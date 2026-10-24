"""
Passively listen on HID collections and print every input report that arrives.

Sending frames to an unknown vendor channel can knock a dongle off the bus
(see the K03S incident in README), so always listen first: many dongles push
battery status on their own.

Usage:
    python listen_hid.py FF80                # 15s on every collection whose
                                             # VID/PID/path/product matches
    python listen_hid.py 4131 --sec 30       # 30s
    python listen_hid.py FF80 --only-in      # only collections that have input

Read length is always the exact InputReportByteLength: asking for one byte
more makes ReadFile fail with err=0 on this driver stack.
"""
import ctypes
import sys
import time
from ctypes import wintypes as w

import dump_hid as D

kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]


kernel32.CreateEventW.restype = w.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
kernel32.ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                              ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(w.DWORD), w.BOOL]
kernel32.CancelIoEx.argtypes = [w.HANDLE, ctypes.c_void_p]
kernel32.ResetEvent.argtypes = [w.HANDLE]


def open_overlapped(path):
    h = kernel32.CreateFileW(ctypes.c_wchar_p(path),
                             D.GENERIC_READ | D.GENERIC_WRITE,
                             D.FILE_SHARE_READ | D.FILE_SHARE_WRITE, None,
                             D.OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
    if not h or h == w.HANDLE(-1).value:
        return None
    return h


def listen(rec, seconds):
    tag = "%04X:%04X %04X/%04X" % (rec["vid"], rec["pid"],
                                   rec.get("page", 0), rec.get("usage", 0))
    n = rec.get("inbox", 0)
    if n <= 0:
        print("%-28s  无输入报文，跳过" % tag)
        return 0
    h = open_overlapped(rec["path"])
    if not h:
        print("%-28s  打开失败 err=%d" % (tag, ctypes.get_last_error()))
        return 0
    ev = kernel32.CreateEventW(None, True, False, None)
    buf = ctypes.create_string_buffer(n)
    hits = 0
    print("%-28s  监听 %ds，读 %d 字节 …" % (tag, seconds, n))
    try:
        ovr = OVERLAPPED()
        ovr.hEvent = ev
        got = w.DWORD(0)
        kernel32.ReadFile(h, ctypes.cast(buf, ctypes.c_void_p), n,
                          ctypes.byref(got), ctypes.byref(ovr))
        deadline = time.time() + seconds
        while True:
            left = deadline - time.time()
            if left <= 0:
                break
            r = kernel32.WaitForSingleObject(ev, int(min(left, 1.0) * 1000))
            if r != WAIT_OBJECT_0:
                continue
            nn = w.DWORD(0)
            kernel32.GetOverlappedResult(h, ctypes.byref(ovr), ctypes.byref(nn),
                                         False)
            hits += 1
            raw = buf.raw[:min(nn.value, n)]
            printable = " ".join("%02X" % b for b in raw[:40])
            print("  [%5.2fs] %3d B  %s" % (time.time() - (deadline - seconds),
                                            nn.value, printable))
            kernel32.CancelIoEx(h, ctypes.byref(ovr))
            kernel32.ResetEvent(ev)
            ovr = OVERLAPPED()
            ovr.hEvent = ev
            got = w.DWORD(0)
            kernel32.ReadFile(h, ctypes.cast(buf, ctypes.c_void_p), n,
                              ctypes.byref(got), ctypes.byref(ovr))
        kernel32.CancelIoEx(h, ctypes.byref(ovr))
    finally:
        kernel32.CloseHandle(ev)
        kernel32.CloseHandle(h)
    print("  合计 %d 帧" % hits)
    return hits


def main():
    want = ""
    sec = 15.0
    only_in = "--only-in" in sys.argv
    for i, a in enumerate(sys.argv[1:], 1):
        if a == "--sec":
            sec = float(sys.argv[i + 1])
        elif not a.startswith("-") and sys.argv[i - 1] != "--sec":
            want = a.upper()
    rows = [r for r in (D.dump(d, False) for d in D.list_devices()) if r]
    picked = []
    for r in rows:
        tag = "%04X:%04X" % (r["vid"], r["pid"])
        hay = " ".join([tag, "%04X/%04X" % (r.get("page", 0), r.get("usage", 0)),
                        r.get("path") or "", r.get("hwid") or "",
                        r.get("prod") or "", r.get("name") or ""]).upper()
        if want and want not in hay:
            continue
        if only_in and r.get("inbox", 0) <= 0:
            continue
        picked.append(r)
    if not picked:
        print("没有匹配 %r 的集合" % want)
        return
    total = 0
    for r in picked:
        total += listen(r, sec)
    print("\n全部集合共收到 %d 帧" % total)


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
