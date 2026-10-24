"""
Generic HID vendor-channel transaction tool for the Actions 10D6:4801 dongle.

Sends 64-byte output reports on one collection and prints any input report
that arrives within the wait window. Used to fingerprint the vendor protocol
before writing a provider.

Usage:
    python hid_xact.py FF90 55650100              # single frame, hex string
    python hid_xact.py FF90 55650100 A55A0100      # several frames
    python hid_xact.py FF90 --sweep 55,65          # sweep byte2 = 00..FF
    python hid_xact.py FF90 --sweep2               # sweep byte0 = 00..FF
    [--wait 0.4]

Frames are zero-padded to the 64-byte report length.
"""
import ctypes
import sys
import time
from ctypes import wintypes as w

kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0

COL = {
    "FF90": r"\\?\hid#vid_10d6&pid_4801&mi_00&col03"
            r"#7&32153eb1&0&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}",
    "FF82": r"\\?\hid#vid_10d6&pid_4801&mi_00&col04"
            r"#7&32153eb1&0&0003#{4d1e55b2-f16f-11cf-88cb-001111000030}",
}


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]


kernel32.CreateFileW.restype = w.HANDLE
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                                 w.DWORD, w.DWORD, w.HANDLE]
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.CreateEventW.restype = w.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
kernel32.ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                              ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                               ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(w.DWORD), w.BOOL]
kernel32.CancelIoEx.argtypes = [w.HANDLE, ctypes.c_void_p]


def hexs(b, n=24):
    return " ".join("%02X" % x for x in b[:n])


def parse_hex(s):
    s = s.replace(" ", "")
    return [int(s[i:i + 2], 16) for i in range(0, len(s), 2)]


def build_sweep(a, b):
    out = []
    for op in range(256):
        f = [0] * 64
        if a is not None:
            f[0], f[1] = a, b
            f[2] = op
        else:
            f[0] = op
        out.append(f)
    return out


def run(name, path, frames, wait):
    h = kernel32.CreateFileW(ctypes.c_wchar_p(path),
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
    if not h or h == w.HANDLE(-1).value:
        print("%s: open failed err=%d" % (name, ctypes.get_last_error()))
        return
    print("=== %s : %d frame(s), wait=%.2fs ===" % (name, len(frames), wait))
    evr = kernel32.CreateEventW(None, True, False, None)
    evw = kernel32.CreateEventW(None, True, False, None)
    rbuf = ctypes.create_string_buffer(72)
    wbuf = ctypes.create_string_buffer(72)
    hits = 0
    try:
        ovr = OVERLAPPED()
        ovr.hEvent = evr
        got = w.DWORD(0)
        armed = kernel32.ReadFile(h, ctypes.cast(rbuf, ctypes.c_void_p), 64,
                                  ctypes.byref(got), ctypes.byref(ovr))
        for f in frames:
            ctypes.memset(wbuf, 0, 72)
            for i, b in enumerate(f):
                wbuf[i] = b
            wrote = w.DWORD(0)
            ovw = OVERLAPPED()
            ovw.hEvent = evw
            ok = kernel32.WriteFile(h, ctypes.cast(wbuf, ctypes.c_void_p), 64,
                                    ctypes.byref(wrote), ctypes.byref(ovw))
            err = ctypes.get_last_error()
            if not ok and err != ERROR_IO_PENDING:
                print("  %-16s write rejected err=%d" % (hexs(f, 6), err))
                continue
            n = w.DWORD(0)
            kernel32.GetOverlappedResult(h, ctypes.byref(ovw), ctypes.byref(n),
                                         True)
            if kernel32.WaitForSingleObject(evr, int(wait * 1000)) != WAIT_OBJECT_0:
                print("  %-16s sent, no reply" % hexs(f, 6))
                continue
            n = w.DWORD(0)
            kernel32.GetOverlappedResult(h, ctypes.byref(ovr), ctypes.byref(n),
                                         False)
            hits += 1
            print("  HIT  sent=%-16s  reply(%d)= %s"
                  % (hexs(f, 6), n.value, hexs(rbuf.raw, 32)))
            # re-arm
            kernel32.CancelIoEx(h, ctypes.byref(ovr))
            ovr = OVERLAPPED()
            ovr.hEvent = evr
            got = w.DWORD(0)
            armed = kernel32.ReadFile(h, ctypes.cast(rbuf, ctypes.c_void_p), 64,
                                      ctypes.byref(got), ctypes.byref(ovr))
        print("  hits = %d" % hits)
    finally:
        kernel32.CancelIoEx(h, ctypes.byref(ovr))
        kernel32.CloseHandle(evr)
        kernel32.CloseHandle(evw)
        kernel32.CloseHandle(h)


def main():
    which = sys.argv[1]
    wait = 0.4
    if "--wait" in sys.argv:
        wait = float(sys.argv[sys.argv.index("--wait") + 1])
    frames = []
    if "--sweep" in sys.argv:
        spec = sys.argv[sys.argv.index("--sweep") + 1]
        a, b = parse_hex(spec)[:2]
        frames = build_sweep(a, b)
    elif "--sweep2" in sys.argv:
        frames = build_sweep(None, None)
    else:
        skip_next = False
        for a in sys.argv[2:]:
            if skip_next:
                skip_next = False
                continue
            if a == "--wait":
                skip_next = True
                continue
            if a.startswith("--") or not all(c in "0123456789abcdefABCDEF" for c in a):
                continue
            f = [0] * 64
            d = parse_hex(a)
            f[:len(d)] = d
            frames.append(f)
    run(which, COL[which], frames, wait)


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
