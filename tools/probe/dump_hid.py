"""
Dump every HID top-level collection: VID/PID, product, usage page/usage,
report lengths, plus the raw report descriptor.

Usage:
    python dump_hid.py                # all devices
    python dump_hid.py 10D6           # only matching VID / path / product
    python dump_hid.py 10D6 -d        # also print the report descriptor

The report descriptor is the key artifact: it tells us whether a Chinese
dongle exposes a vendor-defined collection where battery queries can land.
"""
import ctypes
import sys
from ctypes import wintypes as w

hid = ctypes.WinDLL("hid.dll")
setupapi = ctypes.WinDLL("setupapi.dll")
kernel32 = ctypes.WinDLL("kernel32.dll")

GUID_DEVINTERFACE_HID = "{4D1E55B2-F16F-11CF-88CB-001111000030}"
DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
IOCTL_HID_GET_REPORT_DESCRIPTOR = 0x000B01A0
HIDP_STATUS_SUCCESS = 0x00110000


class GUID(ctypes.Structure):
    _fields_ = [("Data1", w.DWORD), ("Data2", w.WORD), ("Data3", w.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", w.DWORD), ("Reserved", ctypes.POINTER(w.ULONG))]


class SP_DEVINFO_DATA(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("ClassGuid", GUID),
                ("DevInst", w.DWORD), ("Reserved", ctypes.POINTER(w.ULONG))]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Size", w.ULONG), ("VendorID", w.USHORT),
                ("ProductID", w.USHORT), ("VersionNumber", w.USHORT)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", w.USHORT), ("UsagePage", w.USHORT),
                ("InputReportByteLength", w.USHORT),
                ("OutputReportByteLength", w.USHORT),
                ("FeatureReportByteLength", w.USHORT),
                ("Reserved", w.USHORT * 17),
                ("NumberLinkCollectionNodes", w.USHORT),
                ("NumberInputButtonCaps", w.USHORT),
                ("NumberInputValueCaps", w.USHORT),
                ("NumberInputDataIndices", w.USHORT),
                ("NumberOutputButtonCaps", w.USHORT),
                ("NumberOutputValueCaps", w.USHORT),
                ("NumberOutputDataIndices", w.USHORT),
                ("NumberFeatureButtonCaps", w.USHORT),
                ("NumberFeatureValueCaps", w.USHORT),
                ("NumberFeatureDataIndices", w.USHORT)]


SPDRP_DEVICEDESC = 0x00
SPDRP_FRIENDLYNAME = 0x0C
SPDRP_HARDWAREID = 0x01

# 64-bit handles must not be truncated to int by ctypes' default restype
setupapi.SetupDiGetClassDevsW.restype = w.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), w.LPCWSTR,
                                         w.HWND, w.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    w.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), ctypes.POINTER(GUID), w.DWORD,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    w.HANDLE, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), ctypes.c_void_p, w.DWORD,
    ctypes.POINTER(w.DWORD), ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [
    w.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA), w.DWORD,
    ctypes.POINTER(w.DWORD), ctypes.c_void_p, w.DWORD,
    ctypes.POINTER(w.DWORD)]
hid.HidD_GetAttributes.argtypes = [w.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [w.HANDLE,
                                      ctypes.POINTER(ctypes.c_void_p)]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
for _fn in (hid.HidD_GetProductString, hid.HidD_GetManufacturerString,
            hid.HidD_GetSerialNumberString):
    _fn.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]

kernel32.CreateFileW.restype = w.HANDLE
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                                 w.DWORD, w.DWORD, w.HANDLE]
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p,
                                     w.DWORD, ctypes.c_void_p, w.DWORD,
                                     ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.DeviceIoControl.restype = w.BOOL


def guid_from_string(s):
    g = GUID()
    ctypes.oledll.ole32.CLSIDFromString(ctypes.c_wchar_p(s), ctypes.byref(g))
    return g


def get_prop(hdev, did, prop):
    buf = ctypes.create_unicode_buffer(512)
    need = w.DWORD(0)
    if setupapi.SetupDiGetDeviceRegistryPropertyW(hdev, ctypes.byref(did), prop,
                                                  None,
                                                  ctypes.cast(buf, ctypes.c_void_p),
                                                  ctypes.sizeof(buf),
                                                  ctypes.byref(need)):
        return buf.value
    return None


def list_devices():
    g = guid_from_string(GUID_DEVINTERFACE_HID)
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(g), None, None,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    out = []
    i = 0
    while True:
        did = SP_DEVICE_INTERFACE_DATA()
        did.cbSize = ctypes.sizeof(did)
        if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(g),
                                                    i, ctypes.byref(did)):
            break
        need = w.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None,
                                                  0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(w.DWORD)).contents.value = 8
        devinfo = SP_DEVINFO_DATA()
        devinfo.cbSize = ctypes.sizeof(devinfo)
        ok = setupapi.SetupDiGetDeviceInterfaceDetailW(
            hdev, ctypes.byref(did), buf, need.value, ctypes.byref(need),
            ctypes.byref(devinfo))
        path = None
        if ok:
            path = ctypes.wstring_at(ctypes.addressof(buf) + ctypes.sizeof(w.DWORD))
        hwid = get_prop(hdev, devinfo, SPDRP_HARDWAREID)
        name = (get_prop(hdev, devinfo, SPDRP_FRIENDLYNAME)
                or get_prop(hdev, devinfo, SPDRP_DEVICEDESC))
        out.append(dict(path=path, hwid=hwid, name=name))
        i += 1
    kernel32.CloseHandle(hdev)
    return out


def open_dev(path):
    h = kernel32.CreateFileW(ctypes.c_wchar_p(path), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                             OPEN_EXISTING, 0, None)
    if not h or h == w.HANDLE(-1).value:
        return None
    return h


def report_descriptor(h):
    buf = ctypes.create_string_buffer(4096)
    got = w.DWORD(0)
    ok = kernel32.DeviceIoControl(h, IOCTL_HID_GET_REPORT_DESCRIPTOR, None, 0,
                                  ctypes.cast(buf, ctypes.c_void_p), 4096,
                                  ctypes.byref(got), None)
    if not ok:
        return None
    return bytes(buf.raw[:got.value])


def dump(d, with_desc):
    h = open_dev(d["path"])
    if not h:
        return None
    try:
        rec = dict(d)
        attr = HIDD_ATTRIBUTES()
        attr.Size = ctypes.sizeof(attr)
        if hid.HidD_GetAttributes(h, ctypes.byref(attr)):
            rec["vid"] = attr.VendorID
            rec["pid"] = attr.ProductID
            rec["ver"] = attr.VersionNumber
        else:
            rec["vid"] = rec["pid"] = rec["ver"] = 0
        strs = {}
        for fn, label in ((hid.HidD_GetProductString, "prod"),
                          (hid.HidD_GetManufacturerString, "man"),
                          (hid.HidD_GetSerialNumberString, "sn")):
            b = ctypes.create_unicode_buffer(256)
            if fn(h, ctypes.cast(b, ctypes.c_void_p), ctypes.sizeof(b)):
                strs[label] = b.value
        rec.update(strs)
        pp = ctypes.c_void_p()
        if hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            caps = HIDP_CAPS()
            if hid.HidP_GetCaps(pp, ctypes.byref(caps)) == HIDP_STATUS_SUCCESS:
                rec.update(page=caps.UsagePage, usage=caps.Usage,
                           inbox=caps.InputReportByteLength,
                           outbox=caps.OutputReportByteLength,
                           featbox=caps.FeatureReportByteLength,
                           nodes=caps.NumberLinkCollectionNodes,
                           featvals=caps.NumberFeatureValueCaps)
            hid.HidD_FreePreparsedData(pp)
        if with_desc:
            rec["desc"] = report_descriptor(h)
        return rec
    finally:
        kernel32.CloseHandle(h)


def main():
    want = ""
    with_desc = "-d" in sys.argv
    for a in sys.argv[1:]:
        if not a.startswith("-"):
            want = a.upper()
    rows = [r for r in (dump(d, with_desc) for d in list_devices()) if r]
    shown = 0
    for r in rows:
        tag = "%04X:%04X" % (r["vid"], r["pid"])
        hay = " ".join([tag, r.get("path") or "", r.get("hwid") or "",
                        r.get("prod") or "", r.get("name") or ""]).upper()
        if want and want not in hay:
            continue
        shown += 1
        print("%s  ver=%04X  usage=%04X/%04X  in=%d out=%d feat=%d  nodes=%d featvals=%d"
              % (tag, r["ver"], r.get("page", 0), r.get("usage", 0),
                 r.get("inbox", 0), r.get("outbox", 0), r.get("featbox", 0),
                 r.get("nodes", 0), r.get("featvals", 0)))
        print("    name=%r prod=%r man=%r sn=%r"
              % (r.get("name"), r.get("prod"), r.get("man"), r.get("sn")))
        print("    hwid=%s" % r.get("hwid"))
        print("    path=%s" % r.get("path"))
        rd = r.get("desc")
        if with_desc and rd:
            print("    report descriptor (%d bytes):" % len(rd))
            for off in range(0, len(rd), 24):
                print("      %03X: %s" % (off, " ".join("%02X" % b for b in rd[off:off + 24])))
        print()
    print("total %d collection(s), shown %d" % (len(rows), shown))


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
