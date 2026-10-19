"""PNG writer for the screenshots of the serial console (RGB565, little endian)."""
import struct
import zlib


def write_png565(path, width, height, pixels, scale=2):
    rows = []
    for y in range(height):
        line = bytearray()
        for x in range(width):
            c = pixels[2 * (y * width + x)] | (pixels[2 * (y * width + x) + 1] << 8)
            rgb = bytes(((c >> 11) << 3 | (c >> 13), ((c >> 5) & 63) << 2 | ((c >> 9) & 3), (c & 31) << 3 | ((c >> 2) & 7)))
            line += rgb * scale
        rows.extend([b"\0" + bytes(line)] * scale)
    raw = b"".join(rows)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width * scale, height * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
