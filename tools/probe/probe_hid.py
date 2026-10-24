"""
Probe Rapoo (VID_24AE) HID collections: caps, report descriptor, feature report fuzzing.
Pure ctypes, no third-party deps.
"""
import ctypes
from ctypes import wintypes as w

hid = ctypes.WinDLL("hid.dll")
setupapi = ctypes.WinDLL("setupapi.dll")
kernel32 = ctypes.WinDLL("kernel32.dll")

GUID_DEVINTERFACE_HID = "{4D1E55B2-F16F-11CF-88CB-001111000030}"


class GUID(ctypes.Structure):
    _fields_ = [("Data1", w.DWORD), ("Data2", w.WORD), ("Data3", w.WORD),
                ("Data4", ctypes.c_ubyte * 8)]


def guid_from_string(s):
    g = GUID()
    ctypes.oledll.ole32.CLSIDFromString(ctypes.c_wchar_p(s), ctypes.byref(g))
    return g


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


setupapi.SetupDiGetClassDevsW.restype = w.HANDLE
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [w.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA),
                                                ctypes.POINTER(GUID), w.DWORD,
                                                ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [w.HANDLE,
                                                     ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                     ctypes.c_void_p, w.DWORD,
                                                     ctypes.POINTER(w.DWORD),
                                                     ctypes.POINTER(SP_DEVINFO_DATA)]
setupapi.SetupDiGetDeviceRegistryPropertyW.argtypes = [w.HANDLE, ctypes.POINTER(SP_DEVINFO_DATA),
                                                       w.DWORD, ctypes.POINTER(w.DWORD),
                                                       ctypes.c_void_p, w.DWORD,
                                                       ctypes.POINTER(w.DWORD)]
hid.HidD_GetAttributes.argtypes = [w.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetPreparsedData.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_GetProductString.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_GetManufacturerString.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_GetSerialNumberString.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_GetFeature.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_SetFeature.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_GetInputReport.argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]

kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.CloseHandle.restype = w.BOOL
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                                 w.DWORD, w.DWORD, w.HANDLE]
kernel32.CreateFileW.restype = w.HANDLE
kernel32.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD,
                                     ctypes.c_void_p, w.DWORD,
                                     ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.DeviceIoControl.restype = w.BOOL
kernel32.GetLastError.restype = w.DWORD

DIGCF_PRESENT = 0x02
DIGCF_DEVICEINTERFACE = 0x10
SPDRP_DEVICEDESC = 0x0000
SPDRP_HARDWAREID = 0x0001
SPDRP_FRIENDLYNAME = 0x000C

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3

IOCTL_HID_GET_REPORT_DESCRIPTOR = 0x000B01A0  # CTL_CODE(FILE_DEVICE_HID, 1, METHOD_NEITHER, ANY)


def get_prop(hdev, did, prop):
    buf = ctypes.create_unicode_buffer(512)
    needed = w.DWORD(0)
    if setupapi.SetupDiGetDeviceRegistryPropertyW(hdev, ctypes.byref(did), prop, None,
                                                  ctypes.cast(buf, ctypes.c_void_p),
                                                  ctypes.sizeof(buf), ctypes.byref(needed)):
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
        if not setupapi.SetupDiEnumDeviceInterfaces(hdev, None, ctypes.byref(g), i, ctypes.byref(did)):
            break
        need = w.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), None, 0,
                                                  ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        sdd = ctypes.cast(buf, ctypes.POINTER(w.DWORD))
        sdd.contents.value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        devinfo = SP_DEVINFO_DATA()
        devinfo.cbSize = ctypes.sizeof(devinfo)
        ok = setupapi.SetupDiGetDeviceInterfaceDetailW(hdev, ctypes.byref(did), buf, need.value,
                                                       ctypes.byref(need), ctypes.byref(devinfo))
        path = None
        if ok:
            path = ctypes.wstring_at(ctypes.addressof(buf) + ctypes.sizeof(w.DWORD))
        hwids = get_prop(hdev, devinfo, SPDRP_HARDWAREID)
        name = get_prop(hdev, devinfo, SPDRP_FRIENDLYNAME) or get_prop(hdev, devinfo, SPDRP_DEVICEDESC)
        out.append({"path": path, "hwid": hwids, "name": name})
        i += 1
    kernel32.CloseHandle(hdev)
    return out


def open_dev(path):
    h = kernel32.CreateFileW(ctypes.c_wchar_p(path), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
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


def main():
    devs = [d for d in list_devices() if d["hwid"] and "VID_24AE" in d["hwid"].upper()]
    print(f"found {len(devs)} Rapoo HID collections\n")
    for d in devs:
        print("=" * 78)
        print(d["name"])
        print(d["hwid"])
        print(d["path"])
        h = open_dev(d["path"])
        if not h:
            print("  !! open failed, err", kernel32.GetLastError())
            continue
        attr = HIDD_ATTRIBUTES()
        attr.Size = ctypes.sizeof(attr)
        if hid.HidD_GetAttributes(h, ctypes.byref(attr)):
            print(f"  VID={attr.VendorID:04X} PID={attr.ProductID:04X} ver={attr.VersionNumber:04X}")
        for fn, label in ((hid.HidD_GetProductString, "Product"),
                          (hid.HidD_GetManufacturerString, "Vendor"),
                          (hid.HidD_GetSerialNumberString, "Serial")):
            b = ctypes.create_unicode_buffer(256)
            if fn(h, ctypes.cast(b, ctypes.c_void_p), ctypes.sizeof(b)):
                try:
                    print(f"  {label}: {b.value!r}")
                except Exception:
                    pass
        pp = ctypes.c_void_p()
        if hid.HidD_GetPreparsedData(h, ctypes.byref(pp)):
            caps = HIDP_CAPS()
            if hid.HidP_GetCaps(pp, ctypes.byref(caps)) == 0x00110000:  # HIDP_STATUS_SUCCESS
                print(f"  UsagePage=0x{caps.UsagePage:04X} Usage=0x{caps.Usage:04X}")
                print(f"  InLen={caps.InputReportByteLength} OutLen={caps.OutputReportByteLength} FeatLen={caps.FeatureReportByteLength}")
                print(f"  nodes={caps.NumberLinkCollectionNodes} inVals={caps.NumberInputValueCaps} featVals={caps.NumberFeatureValueCaps}")
            hid.HidD_FreePreparsedData(pp)
        rd = report_descriptor(h)
        if rd:
            print(f"  report descriptor ({len(rd)} bytes):")
            for off in range(0, len(rd), 16):
                print("    " + " ".join(f"{b:02X}" for b in rd[off:off + 16]))
        kernel32.CloseHandle(h)


if __name__ == "__main__":
    ctypes.oledll.ole32.CoInitialize(None)
    main()
