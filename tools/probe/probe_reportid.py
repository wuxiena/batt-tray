"""
Read the report IDs a collection actually uses, via HidP_GetValueCaps.

The offline IOCTL for the raw report descriptor returns junk on this driver
stack, but the preparsed data exposes every value cap with its ReportID
field -- which is all we need to frame a vendor-channel command correctly.

Usage:
    python probe_reportid.py 4131
"""
import ctypes
import sys
from ctypes import wintypes as w

import dump_hid as D

hid = ctypes.WinDLL("hid.dll")

HIDP_INPUT = 0
HIDP_OUTPUT = 1
HIDP_FEATURE = 2
HIDP_STATUS_SUCCESS = 0x00110000
USAGE_UNDEFINED = ""


class HIDP_VALUE_CAPS_RANGE(ctypes.Structure):
    _fields_ = [("UsageMin", w.USHORT), ("UsageMax", w.USHORT),
                ("StringMin", w.USHORT), ("StringMax", w.USHORT),
                ("DesignatorMin", w.USHORT), ("DesignatorMax", w.USHORT),
                ("DataIndexMin", w.USHORT), ("DataIndexMax", w.USHORT)]


class HIDP_VALUE_CAPS_NOTRANGE(ctypes.Structure):
    _fields_ = [("Usage", w.USHORT), ("Reserved1", w.USHORT),
                ("StringIndex", w.USHORT), ("Reserved2", w.USHORT),
                ("DesignatorIndex", w.USHORT), ("Reserved3", w.USHORT),
                ("DataIndex", w.USHORT), ("Reserved4", w.USHORT)]


class HIDP_VALUE_CAPS_UNION(ctypes.Union):
    _fields_ = [("Range", HIDP_VALUE_CAPS_RANGE),
                ("NotRange", HIDP_VALUE_CAPS_NOTRANGE)]


class HIDP_VALUE_CAPS(ctypes.Structure):
    _fields_ = [("UsagePage", w.USHORT), ("ReportID", ctypes.c_ubyte),
                ("IsAlias", w.BOOLEAN), ("BitField", w.USHORT),
                ("LinkCollection", w.USHORT), ("LinkUsage", w.USHORT),
                ("LinkUsagePage", w.USHORT), ("IsRange", w.BOOLEAN),
                ("IsStringRange", w.BOOLEAN), ("IsDesignatorRange", w.BOOLEAN),
                ("IsAbsolute", w.BOOLEAN), ("HasNull", w.BOOLEAN),
                ("Reserved", ctypes.c_ubyte), ("BitSize", w.USHORT),
                ("ReportCount", w.USHORT), ("Reserved2", w.USHORT * 5),
                ("UnitsExp", w.ULONG), ("Units", w.ULONG),
                ("LogicalMin", w.LONG), ("LogicalMax", w.LONG),
                ("PhysicalMin", w.LONG), ("PhysicalMax", w.LONG),
                ("u", HIDP_VALUE_CAPS_UNION)]


hid.HidP_GetValueCaps.argtypes = [ctypes.c_int,
                                  ctypes.POINTER(HIDP_VALUE_CAPS),
                                  ctypes.POINTER(w.USHORT),
                                  ctypes.c_void_p]


def caps_of(h):
    pp = ctypes.c_void_p()
    if not hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
        return None
    out = {}
    for label, kind in (("input", HIDP_INPUT), ("output", HIDP_OUTPUT),
                        ("feature", HIDP_FEATURE)):
        n = w.USHORT(0)
        hid.HidP_GetValueCaps(kind, None, ctypes.byref(n), pp)
        if n.value == 0:
            continue
        arr = (HIDP_VALUE_CAPS * n.value)()
        if hid.HidP_GetValueCaps(kind, arr, ctypes.byref(n), pp) != HIDP_STATUS_SUCCESS:
            continue
        out[label] = [(c.ReportID, c.UsagePage, c.BitSize, c.ReportCount,
                       c.LogicalMin, c.LogicalMax) for c in arr]
    hid.HidD_FreePreparsedData(pp)
    return out


def main():
    want = (sys.argv[1] if len(sys.argv) > 1 else "").upper()
    for d in D.list_devices():
        rec = D.dump(d, False)
        if not rec:
            continue
        tag = "%04X:%04X" % (rec["vid"], rec["pid"])
        hay = " ".join([tag, "%04X/%04X" % (rec.get("page", 0), rec.get("usage", 0)),
                        rec.get("path") or ""]).upper()
        if want and want not in hay:
            continue
        h = D.open_dev(rec["path"])
        if not h:
            continue
        try:
            caps = caps_of(h)
        finally:
            D.kernel32.CloseHandle(h)
        if not caps:
            continue
        print("%s usage=%04X/%04X in=%d out=%d"
              % (tag, rec.get("page", 0), rec.get("usage", 0),
                 rec.get("inbox", 0), rec.get("outbox", 0)))
        for label, items in caps.items():
            for rid, page, bits, cnt, lo, hi in items:
                print("   %-7s reportID=0x%02X  usagePage=0x%04X  bits=%d  count=%d  range=%d..%d"
                      % (label, rid, page, bits, cnt, lo, hi))
        print()


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
