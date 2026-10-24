"""Minimal screen grabber (ctypes GDI + stdlib zlib PNG). No deps.
usage: python capture.py out.png [left top width height]
"""
import ctypes, sys, zlib, struct
from ctypes import wintypes as w

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG),
                ("biPlanes", w.WORD), ("biBitCount", w.WORD),
                ("biCompression", w.DWORD), ("biSizeImage", w.DWORD),
                ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG),
                ("biClrUsed", w.DWORD), ("biClrImportant", w.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", w.DWORD * 3)]


def grab(x, y, w_, h_):
    hdc = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w_, h_)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w_, h_, hdc, x, y, SRCCOPY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w_
    bi.bmiHeader.biHeight = -h_
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0

    buf = ctypes.create_string_buffer(w_ * h_ * 4)
    gdi32.GetDIBits(mem, bmp, 0, h_, buf, ctypes.byref(bi), DIB_RGB_COLORS)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, hdc)
    return buf.raw, w_, h_


def write_png(path, raw, w_, h_):
    rows = []
    for yy in range(h_):
        row = bytearray(raw[yy * w_ * 4:(yy + 1) * w_ * 4])
        # BGRA -> RGBA
        row[0::4], row[2::4] = row[2::4], row[0::4]
        rows.append(b"\x00" + bytes(row))
    data = zlib.compress(b"".join(rows), 6)

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w_, h_, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", data)
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "shot.png"
    user32.SetProcessDPIAware()
    if len(sys.argv) >= 6:
        x, y, w_, h_ = map(int, sys.argv[2:6])
    else:
        x, y = 0, 0
        w_ = user32.GetSystemMetrics(0)
        h_ = user32.GetSystemMetrics(1)
    raw, w_, h_ = grab(x, y, w_, h_)
    write_png(out, raw, w_, h_)
    print(f"{out} {w_}x{h_}")


if __name__ == "__main__":
    main()
