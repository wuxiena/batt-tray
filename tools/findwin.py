"""List top-level windows belonging to RapooBattery and dump their rects/state."""
import ctypes
from ctypes import wintypes as w

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()

EnumWindowsProc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)


class RECT(ctypes.Structure):
    _fields_ = [("left", w.LONG), ("top", w.LONG), ("right", w.LONG), ("bottom", w.LONG)]


def get_text(h, fn, buf_size=256):
    buf = ctypes.create_unicode_buffer(buf_size)
    fn(h, buf, buf_size)
    return buf.value


def get_pid(h):
    pid = w.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    return pid.value


def main():
    results = []

    def cb(h, lparam):
        cls = get_text(h, user32.GetClassNameW)
        title = get_text(h, user32.GetWindowTextW)
        if cls == "RapooBatteryWnd":
            r = RECT()
            user32.GetWindowRect(h, ctypes.byref(r))
            results.append((h, title, cls, user32.IsWindowVisible(h), bool(user32.IsWindowEnabled(h)),
                            (r.left, r.top, r.right, r.bottom), get_pid(h)))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    for h, title, cls, vis, en, rect, pid in results:
        print(f"hwnd=0x{h:X} pid={pid} title={title!r} visible={bool(vis)} enabled={en} rect={rect}")
    if not results:
        print("no RapooBatteryWnd windows found")


if __name__ == "__main__":
    main()
