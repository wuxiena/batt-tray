"""
Dump HID value caps (usage page / usage / logical range / report ID) for a
device's collections. Pure read, no frames sent.

Usage:
    python dump_caps.py 10D6          # every collection of that VID/PID
    python dump_caps.py 10D6 000B     # only one usage page
"""
import ctypes
import sys
from ctypes import wintypes as w

import dump_hid as D

hid = ctypes.WinDLL("hid.dll")
hid.HidD_GetPreparsedData.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetValueCaps.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                  ctypes.c_void_p, ctypes.c_void_p]


class CAPS_UNION(ctypes.Union):
    _fields_ = [("Usage", ctypes.c_ushort), ("UsageMin", ctypes.c_ushort),
                ("StringIndex", ctypes.c_ushort), ("DesignatorIndex", ctypes.c_ushort),
                ("DataIndex", ctypes.c_ushort), ("Reserved", ctypes.c_ushort)]


class VALUE_CAPS(ctypes.Structure):
    _fields_ = [("UsagePage", ctypes.c_ushort),
                ("ReportID", ctypes.c_ushort),
                ("LinkCollection", ctypes.c_ushort),
                ("LinkUsagePage", ctypes.c_ushort),
                ("LinkUsage", ctypes.c_ushort),
                ("BitSize", ctypes.c_ushort),
                ("ReportCount", ctypes.c_ushort),
                ("Reserved", ctypes.c_ushort * 5),
                ("UnitExp", ctypes.c_ulong),
                ("Unit", ctypes.c_ulong),
                ("LogicalMin", ctypes.c_long),
                ("LogicalMax", ctypes.c_long),
                ("PhysicalMin", ctypes.c_long),
                ("PhysicalMax", ctypes.c_long),
                ("LinkCollectionUnits", ctypes.c_ushort),
                ("Reserved2", ctypes.c_ushort)]


def main():
    want_vid = int(sys.argv[1], 16)
    want_page = int(sys.argv[2], 16) if len(sys.argv) > 2 else None

    for d in D.list_devices():
        rec = D.dump(d, False)
        if not rec or rec.get("vid") != want_vid:
            continue
        if want_page is not None and rec.get("page") != want_page:
            continue

        print("== %04X:%04X page=%04X usage=%04X in=%d out=%d feat=%d nodes=%d" % (
            rec["vid"], rec["pid"], rec.get("page", 0), rec.get("usage", 0),
            rec.get("inbox", 0), rec.get("outbox", 0), rec.get("featbox", 0),
            rec.get("nodes", 0)))
        print("   prod:", rec.get("prod"), "| hwid:", rec.get("hwid"))

        h = D.open_dev(d["path"])
        if not h:
            print("   open failed")
            continue
        pp = ctypes.c_void_p()
        if not hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            print("   GetPreparsedData failed")
            D.close(h)
            continue

        for kind, label in ((0, "INPUT"), (1, "OUTPUT"), (2, "FEATURE")):
            arr = (VALUE_CAPS * 32)()
            n = hid.HidP_GetValueCaps(pp, kind, ctypes.byref(arr), ctypes.sizeof(arr))
            if n <= 0:
                continue
            print("   %s caps (%d):" % (label, n))
            for i in range(n):
                c = arr[i]
                print("      page=%04X usage=%04X rptID=%d bits=%d count=%d "
                      "logical=%d..%d" % (c.UsagePage, c.Usage, c.ReportID,
                                          c.BitSize, c.ReportCount,
                                          c.LogicalMin, c.LogicalMax))
        hid.HidD_FreePreparsedData(pp)
        D.close(h)
        print()


main()
