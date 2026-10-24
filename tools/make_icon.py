"""Generate app.ico (16/32/48 px) for BattTray - pure stdlib, no PIL."""
import struct, os

GREEN = (0x3C, 0xC8, 0x3C)  # R,G,B


def draw(size, level=88):
    """Return list of rows (top-down) of (r,g,b,a)."""
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    k = size / 32.0

    def rect(x0, y0, x1, y1, col):
        for y in range(int(y0 * k), int(y1 * k) + 1):
            for x in range(int(x0 * k), int(x1 * k) + 1):
                if 0 <= x < size and 0 <= y < size:
                    px[y][x] = col

    col = GREEN + (255,)
    # body outline 2px
    rect(3, 9, 25, 10.9, col)
    rect(3, 21.1, 25, 23, col)
    rect(3, 9, 4.9, 23, col)
    rect(23.1, 9, 25, 23, col)
    # nub
    rect(26, 14, 28, 18, col)
    # fill
    w = 22 - 6
    fill = int(w * level / 100.0)
    rect(6, 12, 6 + fill, 20, col)
    return px


def bmp_entry(px, size):
    # BITMAPINFOHEADER: height doubled, bottom-up XOR bitmap + AND mask
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = px[y][x]
            xor += bytes((b, g, r, a))
    row_bytes = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(row_bytes)
        for x in range(size):
            if px[y][x][3] == 0:
                row[x // 8] |= (0x80 >> (x % 8))
        mask += row
    return hdr + bytes(xor) + bytes(mask)


def main():
    sizes = [16, 32, 48]
    images = [bmp_entry(draw(s), s) for s in sizes]
    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    for s, img in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    for img in images:
        out += img
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
    with open(path, "wb") as f:
        f.write(out)
    print("wrote", path, len(out), "bytes")


if __name__ == "__main__":
    main()
