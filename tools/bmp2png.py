"""Convert the icontest BMP (32bpp, top-down) into a pixel-exact upscaled PNG.

usage: python bmp2png.py preview.bmp out.png [scale]
"""
import struct, sys, zlib


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack("<I", d[10:14])[0]
    w, h = struct.unpack("<ii", d[18:26])
    bpp = struct.unpack("<H", d[28:30])[0]
    top_down = h < 0
    h = abs(h)
    stride = w * bpp // 8
    rows = []
    for y in range(h):
        src = y if top_down else (h - 1 - y)
        line = d[off + src * stride: off + src * stride + stride]
        rows.append(bytes(b for i in range(w) for b in (line[i * 4 + 2], line[i * 4 + 1], line[i * 4])))
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
                           ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    w, h, rows = read_bmp(src)
    out = []
    for r in rows:
        nr = bytearray()
        for x in range(w):
            nr += r[x * 3:(x + 1) * 3] * k
        for _ in range(k):
            out.append(bytes(nr))
    write_png(dst, w * k, h * k, out)
    print("%s %dx%d (x%d)" % (dst, w * k, h * k, k))


if __name__ == "__main__":
    main()
