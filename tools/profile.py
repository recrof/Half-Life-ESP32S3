#!/usr/bin/env python3
"""Per function histogram of the "prof" console command samples.

usage: tools/profile.py <console.log> [build/HalfLifeESP32S3.elf] [top N]

The samples are the "PROF <pc>..." lines (the last run in the log); the PCs
are mapped to functions with addr2line (the IDF environment must be set up:
. tools/idf_env.sh).
"""
import collections
import glob
import os
import subprocess
import sys


def main():
    log = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/HalfLifeESP32S3.elf"
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    runs, pcs = [], []
    for line in open(log, errors="replace"):
        i = line.find("PROF ")
        if i < 0:
            continue
        words = line[i:].split()
        if words[1] == "END":
            runs.append(pcs)
            pcs = []
        else:
            pcs += [int(w, 16) for w in words[1:]
                    if len(w) == 8 and all(c in "0123456789abcdef" for c in w)]
    samples = runs[-1] if runs else pcs
    counts = collections.Counter(samples)
    addrs = sorted(counts)
    out = subprocess.run(["xtensa-esp32s3-elf-addr2line", "-f", "-e", elf] + ["0x%x" % a for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    names = {a: out[2 * k] if 2 * k < len(out) else "?" for k, a in enumerate(addrs)}
    # the ROM functions (libgcc soft double, memcpy...): the ROM ELF of the IDF tools
    rom = [a for a in addrs if 0x40000000 <= a < 0x40060000]
    roms = sorted(glob.glob(os.path.expanduser("~/.platformio/packages/tool-esp-rom-elfs*/esp32s3_rev0_rom.elf"))
                  + glob.glob(os.path.expanduser("~/.espressif/tools/esp-rom-elfs/*/esp32s3_rev0_rom.elf")))
    if rom and roms:
        out = subprocess.run(["xtensa-esp32s3-elf-addr2line", "-f", "-e", roms[0]] + ["0x%x" % a for a in rom],
                             capture_output=True, text=True).stdout.splitlines()
        for k, a in enumerate(rom):
            names[a] = "rom:" + out[2 * k]
    funcs = collections.Counter()
    for a in addrs:
        funcs[names[a]] += counts[a]
    total = len(samples)
    print("%d samples" % total)
    for name, n in funcs.most_common(top):
        print("%5.1f%%  %s" % (100.0 * n / total, name))


if __name__ == "__main__":
    main()
