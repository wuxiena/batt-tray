"""Overlapped (non-blocking) dump of Rapoo vendor HID input reports."""
import ctypes, time, sys
from ctypes import wintypes as w
import probe_hid as P

kernel32 = P.kernel32
kernel32.ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                              ctypes.POINTER(w.DWORD), ctypes.POINTER(P.OVERLAPPED) if hasattr(P, "OVERLAPPED") else ctypes.c_void_p]
kernel32.ReadFile.restype = w.BOOL
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
kernel32.CreateEventW.restype = w.HANDLE
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.WaitForSingleObject.restype = w.DWORD
kernel32.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(w.DWORD), w.BOOL]
kernel32.GetOverlappedResult.restype = w.BOOL
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.ResetEvent.argtypes = [w.HANDLE]
kernel32.ResetEvent.restype = w.BOOL

FILE_FLAG_OVERLAPPED = 0x40000000
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", w.DWORD), ("OffsetHigh", w.DWORD),
                ("hEvent", w.HANDLE)]


def open_overlapped(path):
    h = kernel32.CreateFileW(ctypes.c_wchar_p(path), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, None)
    if not h or h == w.HANDLE(-1).value:
        return None
    return h


def main(seconds):
    devs = [d for d in P.list_devices() if d["hwid"] and "VID_24AE" in d["hwid"].upper()]
    targets = []
    for d in devs:
        low = d["path"].lower()
        for tag in ("col05", "col06", "col04"):
            if tag in low:
                h = open_overlapped(d["path"])
                if h:
                    targets.append((tag.upper(), h))
    print("targets:", [t[0] for t in targets], flush=True)
    if not targets:
        return

    bufs, events, ovs, pending = {}, {}, {}, {}
    for tag, h in targets:
        bufs[tag] = ctypes.create_string_buffer(256)
        events[tag] = kernel32.CreateEventW(None, True, False, None)
        ov = OVERLAPPED()
        ov.hEvent = events[tag]
        ovs[tag] = ov
        pending[tag] = False

    def start_read(tag, h):
        ovs[tag] = OVERLAPPED()
        ovs[tag].hEvent = events[tag]
        kernel32.ResetEvent(events[tag])
        ok = kernel32.ReadFile(h, ctypes.cast(bufs[tag], ctypes.c_void_p), 256, None,
                               ctypes.byref(ovs[tag]))
        pending[tag] = True
        return ok

    for tag, h in targets:
        start_read(tag, h)

    end = time.time() + seconds
    seen = []
    while time.time() < end:
        for tag, h in targets:
            if not pending[tag]:
                continue
            r = kernel32.WaitForSingleObject(events[tag], 50)
            if r == 0:
                got = w.DWORD(0)
                if kernel32.GetOverlappedResult(h, ctypes.byref(ovs[tag]), ctypes.byref(got), False):
                    data = bufs[tag].raw[:got.value]
                    line = f"{tag} len={got.value:2d} " + " ".join(f"{b:02X}" for b in data)
                    if line not in seen:
                        print(line, flush=True)
                        seen.append(line)
                start_read(tag, h)
    for tag, h in targets:
        kernel32.CloseHandle(h)
        kernel32.CloseHandle(events[tag])
    print(f"{len(seen)} distinct reports", flush=True)


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 10.0)
