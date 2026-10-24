"""
Send LiteEMF commander frames to a vendor collection and print the replies.

Frame format (from LiteEMF api_commander.c, short packet):
    A5 <len> <cmd> <data...> <sum>      sum = (sum of all preceding bytes) & 0xFF

Short frame layout is head,len,cmd then data then sum, and cmd_buf[1] holds
the *total* frame length including head and checksum.

Usage:
    python vend_cmd.py FF80/0001 01 05 10      # cmd 0x01, 0x05, 0x10
    python vend_cmd.py FF80/0001 --raw 55650100   # raw payload, no framing
    [--wait 0.5] [--prefix 00]

By default each frame is prefixed with the collection's report ID byte when
the device's output report is one byte longer than the declared count.
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
kernel32.WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD,
                               ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.c_void_p,
                                         ctypes.POINTER(w.DWORD), w.BOOL]
kernel32.CancelIoEx.argtypes = [w.HANDLE, ctypes.c_void_p]


# 已知会改变设备状态的命令，扫盲时一律跳过。
# 0x1A 实测会让 dongle 重新枚举（手柄直接掉线），别碰。
UNSAFE = {0x11, 0x1A, 0x1B, 0x1C, 0x1D, 0x1F} | set(range(0xD0, 0xD8)) | {0xDF}


def frame(cmd, data=b""):
    body = bytes([0xA5, 3 + len(data) + 1, cmd]) + data
    return body + bytes([sum(body) & 0xFF])


def parse(reply):
    """reply 前面有一个 report id 字节，后面是 A5 len cmd data... sum"""
    if len(reply) < 5 or reply[1] != 0xA5:
        return None
    n = reply[2]
    if n < 4 or n > len(reply) - 1:
        return None
    cmd = reply[3]
    data = reply[4:1 + n - 1]
    want = sum(reply[1:1 + n - 1]) & 0xFF
    return cmd, data, reply[1 + n - 1] == want


def hexs(b, n=32):
    return " ".join("%02X" % x for x in b[:n])


def main():
    want = sys.argv[1]
    wait = 0.5
    prefix = 0x00
    frames = []
    args = sys.argv[2:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--wait":
            wait = float(args[i + 1]); i += 2; continue
        if a == "--prefix":
            prefix = int(args[i + 1], 16); i += 2; continue
        if a == "--sweep":
            lo, hi = int(args[i + 1], 16), int(args[i + 2], 16)
            for c in range(lo, hi + 1):
                if c not in UNSAFE:
                    frames.append(frame(c))
            i += 3; continue
        if a == "--raw":
            h = args[i + 1].replace(" ", "")
            frames.append(bytes.fromhex(h)); i += 2; continue
        frames.append(frame(int(a, 16))); i += 1

    target = None
    for d in D.list_devices():
        rec = D.dump(d, False)
        if not rec:
            continue
        tag = "%04X:%04X %04X/%04X" % (rec["vid"], rec["pid"],
                                       rec.get("page", 0), rec.get("usage", 0))
        if want.upper() in tag.upper() or want.upper() in (rec.get("path") or "").upper():
            target = rec
            break
    if not target:
        print("找不到集合 %r" % want)
        return

    outlen = target["outbox"]
    inlen = target["inbox"]
    print("目标 %04X:%04X usage=%04X/%04X  out=%d in=%d"
          % (target["vid"], target["pid"], target["page"], target["usage"],
             outlen, inlen))

    h = kernel32.CreateFileW(ctypes.c_wchar_p(target["path"]),
                             D.GENERIC_READ | D.GENERIC_WRITE,
                             D.FILE_SHARE_READ | D.FILE_SHARE_WRITE, None,
                             D.OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
    if not h or h == w.HANDLE(-1).value:
        print("打开失败 err=%d" % ctypes.get_last_error())
        return
    evr = kernel32.CreateEventW(None, True, False, None)
    evw = kernel32.CreateEventW(None, True, False, None)
    rbuf = ctypes.create_string_buffer(inlen)
    wbuf = ctypes.create_string_buffer(outlen)
    try:
        ovr = OVERLAPPED(); ovr.hEvent = evr
        got = w.DWORD(0)
        kernel32.ReadFile(h, ctypes.cast(rbuf, ctypes.c_void_p), inlen,
                          ctypes.byref(got), ctypes.byref(ovr))
        for f in frames:
            payload = bytes([prefix]) + f
            payload = payload[:outlen].ljust(outlen, b"\x00")
            ctypes.memset(wbuf, 0, outlen)
            ctypes.memmove(wbuf, payload, len(payload))
            ovw = OVERLAPPED(); ovw.hEvent = evw
            wrote = w.DWORD(0)
            ok = kernel32.WriteFile(h, ctypes.cast(wbuf, ctypes.c_void_p), outlen,
                                    ctypes.byref(wrote), ctypes.byref(ovw))
            err = ctypes.get_last_error()
            if not ok and err != ERROR_IO_PENDING:
                print("  发送被拒 err=%d  帧=%s" % (err, hexs(payload, 10)))
                continue
            n = w.DWORD(0)
            kernel32.GetOverlappedResult(h, ctypes.byref(ovw), ctypes.byref(n), True)

            # 每帧都要核对 cmd：上一条命令的应答可能还压在缓冲区里，
            # 不校验就会把旧应答错记成新命令的回复。
            hit = None
            deadline = time.time() + wait
            while True:
                left = deadline - time.time()
                if left <= 0:
                    break
                if kernel32.WaitForSingleObject(evr, int(left * 1000)) != WAIT_OBJECT_0:
                    break
                nn = w.DWORD(0)
                kernel32.GetOverlappedResult(h, ctypes.byref(ovr), ctypes.byref(nn), False)
                raw = rbuf.raw[:nn.value]
                kernel32.CancelIoEx(h, ctypes.byref(ovr))
                ovr = OVERLAPPED(); ovr.hEvent = evr
                got = w.DWORD(0)
                kernel32.ReadFile(h, ctypes.cast(rbuf, ctypes.c_void_p), inlen,
                                  ctypes.byref(got), ctypes.byref(ovr))
                p = parse(raw)
                if p and p[0] == f[2]:
                    hit = (raw, p)
                    break
            if hit:
                raw, (cmd, data, sumok) = hit
                print("  命中 cmd=%02X%s  data(%d)= %s"
                      % (cmd, "" if sumok else " [校验和不符]", len(data), hexs(data, 32)))
            else:
                print("  无应答 cmd=%02X" % f[2])
    finally:
        kernel32.CancelIoEx(h, ctypes.byref(ovr))
        kernel32.CloseHandle(evr)
        kernel32.CloseHandle(evw)
        kernel32.CloseHandle(h)


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
