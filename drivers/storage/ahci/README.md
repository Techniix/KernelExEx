# kexahci.mpd: SATA AHCI for Windows 98 SE / ME

Own GPLv2 driver. Windows 98 has no driver for SATA controllers in AHCI
mode, which most boards since about 2008 use by default (and many cannot
switch to IDE mode): the disks are not there at all. kexahci is an NT SCSI
miniport for Windows 98's SCSIPORT.PDR, like kexata: each SATA port is a
SCSI target, Windows' disk and CD-ROM drivers sit on top.

- Disks: SCSI translated to ATA, READ/WRITE DMA EXT (48-bit) whenever the
  disk has it; up to 2 TB (SCSI's 32-bit block numbers).
- CD/DVD drives on SATA: the SCSI command as an ATAPI packet, by DMA.
- One command at a time per controller, completion polled; the
  controller's interrupt stays off. Ports up to 7 (target 7 is SCSIPORT's).
- Binds to PCI class 01 06 01 (any AHCI controller), Intel ICH9/ICH10 by ID.

## Windows 98 notes (found in testing)

- Windows 98 leaves the AHCI register block (BAR 5) out of the device node's
  resources: only the IDE-style BAR 4 ports come through, and
  `ScsiPortGetDeviceBase` then hands the physical address back unmapped (a
  page fault in SCSIPORT on the first register read). kexahci finds the
  controller on PCI and maps BAR 5 with NTKERN's `MmMapIoSpace`.

## Tests (QEMU, ICH9 AHCI, Korean 98 SE)

FAT32 disk on port 0: the Add New Hardware wizard installs it from
`INF\OTHER`, no restart needed; the disk is D:. 405 DLLs (filling the disk)
copied onto it and compared with the originals on the host, `fsck.fat`
clean; `fc /b` on a copied EXPLORER.EXE.

## Not yet

Interrupts and several commands at once (NCQ), port multipliers, hot plug,
ATAPI tested on hardware, booting from an AHCI disk (the BIOS's own int 13h
handles that until Windows loads; then this driver takes over).
