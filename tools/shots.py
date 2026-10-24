"""Regenerate the docs screenshots.

- docs/tray-icon.png : rendered by the app's own drawing code (tools/icontest.exe),
  three rows = light taskbar / dark taskbar / cached reading
- docs/preview.png   : live capture of the circular float window

The taskbar is usually covered by whatever window is in the foreground, so the
tray strip grab briefly shows the desktop (Win+D), then restores it.

usage: python shots.py [scale]
"""
import ctypes, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture import grab, write_png

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()

KEYUP = 0x0002
VK_LWIN, VK_D = 0x5B, 0x44


def show_desktop():
    user32.keybd_event(VK_LWIN, 0, 0, 0)
    time.sleep(0.03)
    user32.keybd_event(VK_D, 0, 0, 0)
    time.sleep(0.05)
    user32.keybd_event(VK_D, 0, KEYUP, 0)
    time.sleep(0.03)
    user32.keybd_event(VK_LWIN, 0, KEYUP, 0)


def read_png(path):
    """Return (w, h, bpp, rows) for an RGBA/RGB PNG written by capture.py."""
    import struct, zlib
    d = open(path, "rb").read()
    i, idat, w, h, ct = 8, b"", 0, 0, 0
    while i < len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        typ, data = d[i + 4:i + 8], d[i + 8:i + 8 + ln]
        i += 12 + ln
        if typ == b"IHDR":
            w, h, _bd, ct = struct.unpack(">IIBB", data[:10])
        elif typ == b"IDAT":
            idat += data
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride, rows, prev, p = w * bpp, [], bytearray(w * bpp), 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                q = a + b - c
                pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows


def write_rows(dst, w, h, rows):
    import struct, zlib
    raw = b"".join(b"\x00" + r for r in rows)

    def ch(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" +
                          ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                          ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))


def upscale(src, dst, k):
    w, h, bpp, rows = read_png(src)
    out = []
    for r in rows:
        nr = bytearray()
        for x in range(w):
            nr += r[x * bpp:(x + 1) * bpp] * k
        for _ in range(k):
            out.append(bytes(nr))
    write_rows(dst, w * k, h * k, out)
    return w * k, h * k


def crop(raw, w, h, bpp, x, y, cw, ch):
    """Cut a sub-rectangle out of a raw BGRA/RGBA buffer."""
    out = []
    for yy in range(y, y + ch):
        row = raw[(yy * w + x) * bpp:(yy * w + x + cw) * bpp]
        out.append(bytes(row))
    return b"".join(out)


def find_badge(raw, w, h, bpp):
    """Locate the dark rounded badge in a light taskbar strip.

    Returns (x0, x1) of the widest dark column run that is about one icon wide,
    or None when nothing plausible is found.
    """
    dark = []
    for x in range(w):
        n = 0
        for y in range(h):
            o = (y * w + x) * bpp
            if raw[o] < 0x48 and raw[o + 1] < 0x48 and raw[o + 2] < 0x48:
                n += 1
        dark.append(n)

    runs, start = [], None
    for x, n in enumerate(dark):
        if n >= 5:
            if start is None:
                start = x
        elif start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, w - 1))

    best = None
    for a, b in runs:
        wid = b - a + 1
        if 12 <= wid <= 22 and (best is None or wid > best[1] - best[0] + 1):
            best = (a, b)
    return best


def render_icon_sheet(k):
    """Ask icontest.exe for the icon states, then convert to a scaled PNG."""
    exe = os.path.join("tools", "icontest.exe")
    if not os.path.exists(exe):
        print("skip docs/tray-icon.png: tools/icontest.exe not built")
        return
    subprocess.run([os.path.abspath(exe)], cwd="shots", check=True)
    subprocess.run([sys.executable, "tools/bmp2png.py",
                    "shots/preview.bmp", "docs/tray-icon.png", str(k)], check=True)


def main():
    k = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    os.makedirs("shots", exist_ok=True)
    os.makedirs("docs", exist_ok=True)

    # 1) 托盘实拍：任务栏常被前台窗口遮住，先显示桌面
    show_desktop()
    time.sleep(1.8)
    raw, w, h = grab(1480, 1032, 200, 44)
    bpp = 4
    write_png("shots/tray_strip.png", raw, w, h)

    box = find_badge(raw, w, h, bpp)
    if box:
        a, b = box
        print("badge cols %d..%d -> screen x %d..%d" % (a, b, 1480 + a, 1480 + b))
        x0 = max(0, a - 5)
        x1 = min(w - 1, b + 5)
        cw = x1 - x0 + 1
        pad = (44 - 26) // 2
        sub = crop(raw, w, h, bpp, x0, pad, cw, 26)
        write_png("shots/tray_icon_raw.png", sub, cw, 26)

    show_desktop()
    time.sleep(0.6)

    # 2) 图标各状态：由程序自身绘制代码渲染（见 tools/icontest.c）
    render_icon_sheet(k)

    # 3) 圆形浮窗：本身置顶，无需切桌面
    raw, w, h = grab(1826, 906, 66, 66)
    write_png("shots/float_raw.png", raw, w, h)
    fw, fh = upscale("shots/float_raw.png", "docs/preview.png", 5)
    print("docs/preview.png %dx%d" % (fw, fh))


if __name__ == "__main__":
    main()
