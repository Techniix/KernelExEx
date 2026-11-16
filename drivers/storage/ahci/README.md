# kexahci.mpd: SATA AHCI for Windows 98 SE / ME

Own GPLv2 driver. Windows 98 has no driver for SATA controllers in AHCI
mode, which most boards since about 2008 use by default (and many cannot
switch to IDE mode): the disks are not there at all. kexahci is an NT SCSI
miniport for Windows 98's SCSIPORT.PDR, like kexata: each SATA port is a
SCSI target, Windows' disk and CD-ROM drivers sit on top.

- Disks: SCSI translated to ATA, READ/WRITE DMA EXT (48-bit) whenever the
  disk has it; up to 2 TB (SCSI's 32-bit block numbers).
- CD/DVD drives on SATA: the SCSI command as an ATAPI packet, by DMA.
- Commands complete in the controller's interrupt once Windows turns it on
  (before that, during startup, they are waited for). Ports work in
  parallel. Disks with NCQ take up to 8 reads and writes at once (READ/WRITE
  FPDMA QUEUED); requests to overlapping sectors with a write among them
  keep their order, and anything other than a read or write waits until the
  queue is empty. If a queued command fails, the requests are sent again one
  at a time and the disk carries on without NCQ.
- Ports up to 7 (target 7 is SCSIPORT's).
- `AdapterSettings`: `POLL` (no interrupts), `NONCQ` (no queuing).
- Binds to PCI class 01 06 01 (any AHCI controller), Intel ICH9/ICH10 by ID.

## Windows 98 notes (found in testing)

- Windows 98 leaves the AHCI register block (BAR 5) out of the device node's
  resources: only the IDE-style BAR 4 ports come through, and
  `ScsiPortGetDeviceBase` then hands the physical address back unmapped (a
  page fault in SCSIPORT on the first register read). kexahci finds the
  controller on PCI and maps BAR 5 with NTKERN's `MmMapIoSpace`.
- Windows 98's SCSIPORT sends a disk more requests while one is running
  only when asked with `NextLuRequest` (and `MultipleRequestPerLu` set), and
  it calls back into StartIo from inside a notification: like
  `NextRequest`, `NextLuRequest` is only sent from the outermost call (sent
  from the interrupt handler, it froze Windows).

## Tests (QEMU, ICH9 AHCI, Korean 98 SE)

FAT32 disk on port 0: the Add New Hardware wizard installs it from
`INF\OTHER`, no restart needed; the disk is D:. 405 DLLs (filling the disk)
copied onto it and compared with the originals on the host, `fsck.fat`
clean; `fc /b` on a copied EXPLORER.EXE.

With interrupts and NCQ: two disks and a SATA DVD drive on the controller;
CAB files from the DVD and the IDE CD copied to one disk while system DLLs
fill the other: 425 files compared byte for byte with the originals, no
differences, up to 4 commands in flight per disk.

## What speed to expect

Estimates, not measurements yet: the test machine here emulates a Pentium II
in software, so its numbers say nothing about real hardware. They will be
replaced with figures from real machines.

Windows 98 itself sets the ceiling well below what a modern drive can do.
Every file read and write passes through VCACHE (one more memory copy) and
reaches the driver in pieces of 32 to 64 KB (writes arrive as 32 KB
requests in testing), each paying the cost of several driver layers. A
faster CPU helps more than a faster drive.

| Drive | Large files (copying, reading) | Compared with before |
|---|---|---|
| IDE hard disk (kexata) | what the drive does, roughly 30 to 70 MB/s | unchanged |
| SATA hard disk (kexahci) | what the drive does, roughly 80 to 180 MB/s | was not usable in AHCI mode |
| SATA SSD (kexahci) | very roughly 100 to 300 MB/s, depending on the CPU | was not usable in AHCI mode |
| NVMe SSD (kexnvme) | very roughly 150 to 400 MB/s; held back by Windows 98, not the SSD | was not usable at all |
| SATA or NVMe on a PCI add-in card | at most about 100 MB/s: the 32-bit/33 MHz PCI bus carries 133 MB/s in total | |

The difference users feel most is not the large-file figure but access
time: an SSD answers in about 0.1 ms where a hard disk needs about 10 ms,
so starting Windows and programs gets much faster even where copying does
not.

A simple check on real hardware: time a copy of a file larger than the
installed RAM (so VCACHE cannot hold it) from the drive to `NUL` (reading)
and from a RAM-sized source onto the drive (writing).


## Not yet

Port multipliers, hot plug,
ATAPI tested on hardware, booting from an AHCI disk (the BIOS's own int 13h
handles that until Windows loads; then this driver takes over).
