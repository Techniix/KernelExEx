Functions available to WDM drivers on Windows 98 SE (4.10.2222).

usbd.sys, hidclass.sys, hidparse.sys: export tables of those files.
ntoskrnl.exe, hal.dll: NTKERN.VXD implements these imports itself; the
lists are the kernel-API names found in its name table (strings that look
like Ke*/Io*/Ex*/Rtl*/Mm*/... ), so they may include a few names that are
not real exports. Cross-check against a driver Microsoft shipped for 98 SE
(hidusb.sys imports only names on these lists).
Every list also contains each function that a driver Microsoft shipped on the
98 SE CD imports from that module (all *.sys files in the CAB set), so those
names are proven to resolve. That matters: NTKERN keeps some names in a form
the string scan does not see. KeInitializeDpc, KeInsertQueueDpc, KeSetTimer,
KeInitializeTimer and KeCancelTimer, for example, are only on the lists
because 1394bus.sys, acpi.sys, aha894x.sys and others import them.
ndis.sys, ks.sys, portcls.sys, stream.sys, wmilib.sys, usbcamd.sys and
dxapi.sys have proven lists only.

Not seen in any 98 SE driver (treat as absent): IoAllocateWorkItem,
IoQueueWorkItem (use ExQueueWorkItem), MmMapLockedPagesSpecifyCache.
