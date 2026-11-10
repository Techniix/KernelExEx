# kexpad.sys

A WDM HID minidriver for USB game pads that speak the Xbox 360 protocol
(vendor class FF, subclass 5D, protocol 01), for Windows 98 SE and ME. It
does the same job as Microsoft's `hidusb.sys`: it reads the pad's interrupt
endpoint and hands HIDCLASS a standard HID game pad report. DirectInput, the
joystick API and KernelExEx's XInput then see the pad like any other.

| File | Purpose |
|---|---|
| `kexpad.c` | Driver: PnP start, configuration, read loop, LEDs |
| `x360.c`, `x360.h` | Report descriptor and 360-to-HID translation (host-tested) |
| `hidmini.h` | HID minidriver interface (HIDCLASS) |
| `imp/*.def` | Import lists; `usbd98.def` needs the decorated names 98 SE exports |
| `gen_inf.py` | Makes `kexpad.inf` from Linux `xpad.c` (XTYPE_XBOX360 entries) |
| `kexpad.inf` | Generated; 173 pads plus the class/subclass/protocol compatible ID |

Build: `python3 toolchain/wdm/build_kexpad.py`. The build fails if any import
is missing from the 98 SE export tables (`net/tools/exports/win98se-wdm`) or
if FPU or SIMD instructions appear.

## Report

13 bytes: buttons 1-11 (A B X Y LB RB Back Start LS RS Guide), a 4-bit hat,
X/Y = left stick, Rx/Ry = right stick (signed 16-bit, Y up = negative), then
Z = left trigger and Rz = right trigger.

## Open items

- Not yet run on hardware. 86Box and QEMU cannot pass a USB pad through
  here, so the first test is on real hardware.
- Check which joystick axes (Z/R/U/V) Windows' HID joystick mapping gives
  the right stick and the triggers, then add a matching line to
  `input/gamepad/gamecontrollerdb.txt` if the default layout does not fit.
- Rumble: the descriptor has no output report yet, so `IOCTL_HID_WRITE_REPORT`
  (already handled) is never sent. Needs an output report or a private IOCTL
  for XInputSetState.
- Xbox 360 wireless receiver (different protocol), Xbox One/Series pads.
