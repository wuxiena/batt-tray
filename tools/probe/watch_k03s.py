"""
watch_k03s.py - 持续监听 10D6（西伯利亚 K03S）的全部 HID 集合，
并周期性 ping FF90 厂商通道，观察耳机开关机时 USB 侧的行为差异。

纯诊断工具：只做「读」和「发已知查询帧（55 65 01，和程序里一样）」，
不发随机 opcode，避免把 dongle 弄掉线。

用法:
    python watch_k03s.py 1800        # 跑 30 分钟
输出逐行 flush 到 stdout（重定向到文件随时可看）。
"""
import ctypes
import sys
import threading
import time
from ctypes import wintypes as w

import dump_hid as D

kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258

kernel32.CreateEventW.restype = w.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, w.BOOL, w.BOOL, w.LPCWSTR]
kernel32.ReadFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                              ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                               ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(w.DWORD), w.BOOL]
kernel32.CreateFileW.restype = w.HANDLE


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", w.DWORD), ("OffsetHigh", w.DWORD), ("hEvent", w.HANDLE)]


def log(fmt, *a):
    t = time.strftime("%H:%M:%S")
    print(t, fmt % a, flush=True)


def open_overlapped(path):
    return kernel32.CreateFileW(ctypes.c_wchar_p(path),
                                D.GENERIC_READ | D.GENERIC_WRITE,
                                D.FILE_SHARE_READ | D.FILE_SHARE_WRITE, None,
                                D.OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)


def find_collections(vid):
    out = []
    for d in D.list_devices():
        rec = D.dump(d, False)
        if not rec or rec.get("vid") != vid:
            continue
        out.append(rec)
    return out


def listener(rec, stop):
    """被动监听一个集合：收到帧就打印，然后重新挂起读。"""
    path = rec["path"]
    inlen = rec["inbox"]
    tag = "%04X/%04X" % (rec.get("page", 0), rec.get("usage", 0))
    h = open_overlapped(path)
    if not h or h == w.HANDLE(-1).value:
        log("%s 打开失败", tag)
        return
    ev = kernel32.CreateEventW(None, True, False, None)
    buf = ctypes.create_string_buffer(inlen)
    log("%s 开始监听 (in=%d)", tag, inlen)
    while not stop.is_set():
        ov = OVERLAPPED()
        ov.hEvent = ev
        got = w.DWORD(0)
        ok = kernel32.ReadFile(h, ctypes.cast(buf, ctypes.c_void_p), inlen,
                               ctypes.byref(got), ctypes.byref(ov))
        if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
            log("%s 读被拒 err=%d", tag, ctypes.get_last_error())
            break
        n = w.DWORD(0)
        if not kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True):
            log("%s 读失败 err=%d", tag, ctypes.get_last_error())
            break
        data = bytes(buf.raw[:n.value])
        log("%s 收到 %d 字节: %s", tag, n.value,
            " ".join("%02X" % b for b in data[:16]))
    kernel32.CloseHandle(h)
    log("%s 监听结束", tag)


def pinger(rec, stop, period=20.0):
    """周期性发 FF90 查询帧（55 65 01），看有没有应答。"""
    path = rec["path"]
    outlen = rec["outbox"]
    inlen = rec["inbox"]
    tag = "%04X/%04X" % (rec.get("page", 0), rec.get("usage", 0))
    h = open_overlapped(path)
    if not h or h == w.HANDLE(-1).value:
        log("%s ping 打开失败", tag)
        return
    ev = kernel32.CreateEventW(None, True, False, None)
    log("%s 开始 ping（每 %.0f 秒）", tag, period)
    while not stop.is_set():
        time.sleep(period)
        if stop.is_set():
            break
        # 写 55 65 01
        payload = bytes([0x55, 0x65, 0x01]).ljust(outlen, b"\x00")
        wbuf = ctypes.create_string_buffer(payload)
        written = w.DWORD(0)
        ok = kernel32.WriteFile(h, ctypes.cast(wbuf, ctypes.c_void_p), outlen,
                                ctypes.byref(written), None)
        if not ok:
            log("%s 写被拒 err=%d", tag, ctypes.get_last_error())
            continue
        # 读回应，最多等 400ms
        rbuf = ctypes.create_string_buffer(inlen)
        ov = OVERLAPPED()
        ov.hEvent = ev
        got = w.DWORD(0)
        ok = kernel32.ReadFile(h, ctypes.cast(rbuf, ctypes.c_void_p), inlen,
                               ctypes.byref(got), ctypes.byref(ov))
        if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
            log("%s 读被拒 err=%d", tag, ctypes.get_last_error())
            continue
        wr = kernel32.WaitForSingleObject(ev, 400)
        if wr != WAIT_OBJECT_0:
            log("%s ping 无应答（超时）", tag)
            kernel32.CancelIoEx(h, ctypes.byref(ov))
            continue
        n = w.DWORD(0)
        kernel32.GetOverlappedResult(h, ctypes.byref(ov), ctypes.byref(n), True)
        data = bytes(rbuf.raw[:n.value])
        log("%s ping 有应答 %d 字节: %s", tag, n.value,
            " ".join("%02X" % b for b in data[:16]))
    kernel32.CloseHandle(h)
    log("%s ping 结束", tag)


def main():
    total = float(sys.argv[1]) if len(sys.argv) > 1 else 900.0
    vid = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x10D6

    recs = find_collections(vid)
    if not recs:
        print("找不到 %04X 的任何 HID 集合" % vid)
        return
    for r in recs:
        log("集合 %04X:%04X %04X/%04X in=%d out=%d", r["vid"], r["pid"],
            r.get("page", 0), r.get("usage", 0), r["inbox"], r["outbox"])

    stop = threading.Event()
    threads = []
    for r in recs:
        page = r.get("page", 0)
        if page == 0xFF90:
            t = threading.Thread(target=pinger, args=(r, stop), daemon=True)
        else:
            t = threading.Thread(target=listener, args=(r, stop), daemon=True)
        t.start()
        threads.append(t)

    end = time.time() + total
    while time.time() < end:
        time.sleep(1)
    stop.set()
    time.sleep(1)
    log("监控结束")


main()
