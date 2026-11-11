# kexehci.sys: USB 2.0 for Windows 98 SE / ME

Own GPLv2 driver, written from the EHCI 1.0 and USB 2.0 specifications.

## How it fits into Windows 98

Windows 98 SE has USB 1.1 only. Every EHCI controller of the time shares its
ports with USB 1.1 companion controllers (UHCI or OHCI). kexehci takes all
ports (CONFIGFLAG), resets each new device, and:

- full- and low-speed devices on a root port go back to the companion
  (PORT_OWNER), where Windows' uhcd/openhci, usbhub and class drivers run
  them exactly as before;
- high-speed devices stay. kexehci is their whole stack: host controller,
  root hub, hubs, and the bus driver. It creates one PDO per device with the
  IDs Windows 98's usbhub.sys uses (`USB\VID_xxxx&PID_xxxx&REV_xxxx`,
  `USB\CLASS_xx&SUBCLASS_xx&PROT_xx`, ...; instance from the serial number,
  else the port path) and accepts the same URBs and IOCTLs, so the same INF
  and the same function driver load on either stack.

USB 2.0 hubs are handled inside the driver (no hub PDO); full/low-speed
devices behind them use the hub's transaction translator (split
transactions). Intel's later chipsets (5 series on) have no companions and an
internal "rate matching" hub, which is that case too.

The USBDI bus interface (`IRP_MN_QUERY_INTERFACE`, version 1) tells drivers
whether their device runs at high speed; kexbsd's `kexusb_high_speed()`
asks it, so the BSD drivers (kexrtwn and the rest) use their USB 2.0
settings.

## Design notes

- One QH per endpoint; transfers are appended with the dummy-qTD method
  (the endpoint's dummy becomes the new first qTD and is activated last).
- Interrupt endpoints hang off a tree of halted anchor QHs (32, 16, ... 1
  frames). Split interrupt: start split uframe 0, complete splits 2-4.
- Cancelling and closing happen on the hub thread: unlink the QH, wait for
  the async advance doorbell (or a frame), relink what remains.
- A transfer sent to an unplugged device waits until it is cancelled or the
  device is removed: failing it at once makes hidclass resubmit from its
  completion routine until the stack overflows (seen in QEMU).
- The controller is handed back at shutdown (CONFIGFLAG 0) for the BIOS and
  DOS; the BIOS hands it over at start (USBLEGSUP).
- The INF also installs Windows' usbd.sys (from the Windows CD) when no USB
  1.1 controller installed it yet: Windows' class drivers import it.
- Log: `C:\WINDOWS\KEXUSB2.LOG`, written 20 s after start and then every
  10 s when it changed.

## Not yet

Isochronous transfers (USB audio, webcams), composite devices as one PDO per
interface (Windows 98's `&MI_xx` IDs), suspend.

## Tests

`tests/usb/kexusbt.sys` (build with `build_kexusb.py --tests`) binds to
QEMU's USB disk and runs INQUIRY, READ CAPACITY, a timed 4 MB read and a
64 KB write/read-back. QEMU: `-device ich9-usb-ehci1,id=ehci` plus
`-device usb-tablet,bus=ehci.0` (high speed by default) or
`-device usb-storage,bus=ehci.0,drive=...`; companions with
`-device ich9-usb-uhci1,masterbus=ehci.0,firstport=0` and so on.
