"""
Dump the raw HID report descriptor for a device's collections.

Pure read: opens each matching collection and pulls the descriptor with
IOCTL_HID_GET_REPORT_DESCRIPTOR. No frames are sent, so it is safe on dongles
that re-enumerate when they receive garbage.

Usage:
    python dump_desc.py 10D6          # every collection of that VID/PID
    python dump_desc.py 10D6 000B     # only one usage page
"""
import sys

import dump_hid as D


def main():
    want_vid = int(sys.argv[1], 16)
    want_page = int(sys.argv[2], 16) if len(sys.argv) > 2 else None

    for d in D.list_devices():
        rec = D.dump(d, True)
        if not rec or rec.get("vid") != want_vid:
            continue
        if want_page is not None and rec.get("page") != want_page:
            continue

        print("== %04X:%04X page=%04X usage=%04X in=%d out=%d feat=%d" % (
            rec["vid"], rec["pid"], rec.get("page", 0), rec.get("usage", 0),
            rec.get("inbox", 0), rec.get("outbox", 0), rec.get("featbox", 0)))
        print("   prod:", rec.get("prod"), "| hwid:", rec.get("hwid"))
        print("   path:", rec.get("path"))
        raw = rec.get("desc")
        if not raw:
            print("   descriptor read failed")
            continue
        print("   descriptor %d bytes:" % len(raw))
        for i in range(0, len(raw), 16):
            chunk = raw[i:i + 16]
            print("     %04x  %s" % (i, " ".join("%02x" % b for b in chunk)))
        print()


main()
