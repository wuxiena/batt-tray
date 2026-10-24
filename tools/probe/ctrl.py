# ctrl.py - 给运行中的 BattTray 窗口发命令（调试用）
import ctypes
import ctypes.wintypes as w
import sys

user32 = ctypes.WinDLL("user32.dll", use_last_error=True)
user32.FindWindowW.restype = w.HWND
user32.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
user32.SendMessageW.restype = ctypes.c_long
user32.SendMessageW.argtypes = [w.HWND, w.UINT, ctypes.c_long, ctypes.c_long]

hwnd = user32.FindWindowW("BattTrayWnd", "BattTray")
print("hwnd =", hwnd, "err =", ctypes.get_last_error())
if not hwnd:
    sys.exit(1)

for name, cmd in (("rescan", 4), ("diag", 7)):
    r = user32.SendMessageW(hwnd, 0x0111, cmd, 0)   # WM_COMMAND
    print("sent", name, "cmd =", cmd, "->", r)
