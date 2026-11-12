# kexumass.sys: USB mass storage for Windows 98 SE / ME

Own GPLv2 driver for USB flash drives, disks, card readers and CD/DVD
drives (Bulk-Only Transport; SCSI, ATAPI and UFI command sets).

Windows 98 SE ships the upper half: NTMAP ("1394/USB Disk": ntmap.inf,
ntmap.sys, ntmaphlp.pdr) connects WDM storage devices to Windows' disk layer
(IOS), as it does for 1394 disks with sbp2port.sys. kexumass is the function
driver for the USB interface, with ntmap.sys as its upper filter (the INF
sets `UpperFilters`, as sbp2.inf does), and the bus driver for one child per
logical unit with the IDs ntmap.inf takes (`USBSTOR\GenDisk`, `GenCdRom`,
`GenOptical`). Each SRB runs as one Bulk-Only command on a worker thread.

ntmap.sys and ntmaphlp.pdr come from the Windows 98 SE CD; Windows asks for
it when it does not find them.

See the comment at the top of kexumass.c for what testing on Windows 98 SE
found. Not yet: CBI transport (USB floppy drives), several commands queued
at once (Bulk-Only allows one).
