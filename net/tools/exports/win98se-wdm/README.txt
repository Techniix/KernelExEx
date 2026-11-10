Functions available to WDM drivers on Windows 98 SE (4.10.2222).

usbd.sys, hidclass.sys, hidparse.sys: export tables of those files.
ntoskrnl.exe, hal.dll: NTKERN.VXD implements these imports itself; the
lists are the kernel-API names found in its name table (strings that look
like Ke*/Io*/Ex*/Rtl*/Mm*/... ), so they may include a few names that are
not real exports. Cross-check against a driver Microsoft shipped for 98 SE
(hidusb.sys imports only names on these lists).
Absent on 98 SE, for example: IoAllocateWorkItem, IoQueueWorkItem, KeSetTimer.
