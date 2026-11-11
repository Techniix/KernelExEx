#!/usr/bin/env python3
"""usbdevs -> dev/usb/usbdevs.h (USB_VENDOR_x / USB_PRODUCT_x_y), our own generator."""
import re, sys
from pathlib import Path
here = Path(__file__).parent / "dev" / "usb"
out = ["/* generated from usbdevs by gen_usbdevs.py; do not edit */", "#ifndef KEXFBSD_USBDEVS_H", "#define KEXFBSD_USBDEVS_H"]
for line in (here / "usbdevs").read_text(errors="replace").splitlines():
    m = re.match(r"^vendor\s+(\S+)\s+(0x[0-9a-fA-F]+)", line)
    if m:
        out.append(f"#define USB_VENDOR_{m[1]} {m[2]}")
        continue
    m = re.match(r"^product\s+(\S+)\s+(\S+)\s+(0x[0-9a-fA-F]+)", line)
    if m:
        out.append(f"#define USB_PRODUCT_{m[1]}_{m[2]} {m[3]}")
out.append("#endif")
(here / "usbdevs.h").write_text("\n".join(out) + "\n")
