#!/usr/bin/env python3
"""Builds the flash model pack of the ESP32-S3 Half-Life port.

The studio models (.mdl) and sprites (.spr) are used in place by the engine
(Platform_FindAsset): their data and texture pixels are not copied to PSRAM. The models with a separate texture file (xxxT.mdl)
are merged with it, as the engine does when it loads them. The pack holds the models that the
most maps load (tools/modeluse.txt and spruse.txt: count and name per line, from the map sweep
logs) and their sequence groups (xxx01.mdl...), up to
the size of the flash region; the others are loaded from the SD card as usual.

usage: mkassets.py [--size BYTES] [--list modeluse.txt] VALVEDIR OUT.bin
       then: tools/flash.sh assets   (or the host build: HL_ASSETS=OUT.bin)

Pack format (little endian):
  header (32 bytes): "HLAS", version (1), number of entries, directory offset, total size
  directory: entries of name (56 bytes, lower case, sorted), offset, size
  data: 16-byte aligned
"""
import argparse
import os
import struct
import sys

MAGIC = b"HLAS"
VERSION = 1
ALIGN = 16
NAME_LEN = 56
HEADER = struct.Struct("<4sIIII12x")
ENTRY = struct.Struct("<%dsII" % NAME_LEN)
TEXTURE = struct.Struct("<64siiii")     # mstudiotexture_t: name, flags, width, height, index


def merge_textures(main, tfile):
    """main model + its xxxT.mdl texture model, as Mod_LoadStudioModel merges them"""
    length = struct.unpack_from("<i", main, 72)[0]
    numtex, texindex, texdata = struct.unpack_from("<iii", tfile, 180)
    numskinref, numskinfam, skinindex = struct.unpack_from("<iii", tfile, 192)
    size1 = numtex * TEXTURE.size
    size2 = numskinfam * numskinref * 2
    textures = bytearray(tfile[texindex:texindex + size1])
    skins = tfile[skinindex:skinindex + size2]
    pixels = tfile[texdata:]
    newdata = length + size1 + size2
    for i in range(numtex):
        name, flags, w, h, index = TEXTURE.unpack_from(textures, i * TEXTURE.size)
        TEXTURE.pack_into(textures, i * TEXTURE.size, name, flags, w, h, index - texdata + newdata)
    out = bytearray(main[:length]) + textures + skins + pixels
    struct.pack_into("<i", out, 72, len(out))                                      # length
    struct.pack_into("<iii", out, 180, numtex, length, newdata)                    # textures
    struct.pack_into("<iii", out, 192, numskinref, numskinfam, length + size1)     # skins
    return bytes(out)


def load_file(valve, name):
    path = os.path.join(valve, name)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        data = f.read()
    if not name.endswith(".mdl"):
        return data     # sprites are used as they are
    if data[:4] == b"IDSQ":
        return data     # sequence group
    if data[:4] != b"IDST":
        return None
    numtex = struct.unpack_from("<i", data, 180)[0]
    if numtex == 0:
        tname = os.path.join(valve, name[:-4] + "T.mdl")
        if not os.path.exists(tname):
            return None
        with open(tname, "rb") as f:
            data = merge_textures(data, f.read())
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x6C0000)
    ap.add_argument("--list", action="append",
                    help="usage lists (count and name per line), default: modeluse.txt and spruse.txt")
    ap.add_argument("valve")
    ap.add_argument("out")
    args = ap.parse_args()

    lists = args.list or [os.path.join(os.path.dirname(__file__), n) for n in ("modeluse.txt", "spruse.txt")]
    names = []
    for path in lists:
        for line in open(path):
            count, name = line.split()
            names.append((-int(count), os.path.getsize(os.path.join(args.valve, name)) if os.path.exists(os.path.join(args.valve, name)) else 0, name.lower()))
    # the sequence groups of a model (xxx01.mdl...: animations loaded when they are played) are
    # used as often as the model
    for count, _, name in list(names):
        if not name.endswith(".mdl"):
            continue
        for i in range(1, 100):
            group = "%s%02d.mdl" % (name[:-4], i)
            path = os.path.join(args.valve, group)
            if not os.path.exists(path):
                break
            names.append((count, os.path.getsize(path), group))
    names.sort()    # the most used first, then the smallest

    entries = {}
    used = HEADER.size
    for _, _, name in names:
        data = load_file(args.valve, name)
        if data is None:
            continue
        size = used + ENTRY.size * (len(entries) + 1) + (len(data) + ALIGN - 1) // ALIGN * ALIGN + ALIGN
        if size > args.size:
            continue    # a smaller model may still fit
        entries[name] = data
        used += (len(data) + ALIGN - 1) // ALIGN * ALIGN

    order = sorted(entries)
    diroffset = HEADER.size
    offset = (diroffset + ENTRY.size * len(order) + ALIGN - 1) // ALIGN * ALIGN
    directory = b""
    blob = b""
    for name in order:
        data = entries[name]
        directory += ENTRY.pack(name.encode(), offset + len(blob), len(data))
        blob += data + b"\0" * (-len(data) % ALIGN)
    total = offset + len(blob)
    with open(args.out, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, len(order), diroffset, total))
        f.write(directory)
        f.write(b"\0" * (offset - diroffset - len(directory)))
        f.write(blob)
    skipped = len(names) - len(order)
    print("%d files, %.2f MB (%d not included)" % (len(order), total / 1048576, skipped))


if __name__ == "__main__":
    main()
