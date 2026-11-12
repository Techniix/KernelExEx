# kexata.mpd: IDE/ATA and ATAPI with 48-bit LBA for Windows 98 SE / ME

Own GPLv2 driver. Windows 98's ESDI_506.PDR speaks 28-bit LBA only: on a
disk over 128 GB (2^28 sectors) it wraps around past that point and writes
over the start of the disk. kexata replaces it.

## How it fits

kexata is an NT SCSI miniport. Windows 98 SE's SCSIPORT.PDR loads those
(`DevLoader=*IOS`, `PortDriver=kexata.mpd`; imports only the functions in
`net/tools/exports/win98se-wdm/SCSIPORT.SYS.txt`). Windows' own disk and
CD-ROM drivers sit on top and send SCSI commands:

- disks: SCSI is translated to ATA (INQUIRY, READ CAPACITY, READ/WRITE
  6/10/12, SYNCHRONIZE CACHE as FLUSH CACHE, REQUEST SENSE from the last
  ATA error, ...). Anything past sector 2^28, or longer than 256 sectors,
  uses the 48-bit commands. SCSI's 32-bit block numbers reach 2 TB; READ
  CAPACITY reports at most that.
- CD/DVD drives: the SCSI command goes as an ATAPI packet (PIO).

It binds to each IDE channel's device node (`MF\GOODPRIMARY`,
`MF\GOODSECONDARY`), which Windows makes from the PCI IDE controller
(mshdc.inf, or KernelExEx's kexide.inf for chipsets Windows does not know),
with the channel's legacy ports and IRQ. The bus master registers come from
the controller's BAR 4, found on PCI. One adapter per channel; one command
at a time per channel.

DMA (bus master) is used when the BIOS selected a DMA mode on the drive, so
the chipset timings are set. Otherwise PIO, polled in StartIo with the
drive's interrupt off. `AdapterSettings` in the channel's driver key:
`DMA=OFF` forces PIO; `DMA=FORCE` sets multiword DMA 2 on drives the BIOS
left in PIO (for QEMU, whose BIOS selects no mode).

## Windows 98 SCSIPORT notes (found in testing)

- `ACCESS_RANGE` entries past the device node's resources hold garbage:
  entries longer than 256 ports are ignored.
- SCSIPORT does not hold off the miniport's interrupt while StartIo runs,
  and NextRequest calls StartIo at once. Calling NextRequest from StartIo
  or the ISR therefore nests one level per request (a PIO write's interrupt
  comes as soon as the first sector is in), until a triple fault. StartIo
  and the ISR only mark "finished"; the outermost one asks for the next
  request in a loop (`enter`/`leave`).
- `AdapterSettings` reaches HwFindAdapter as the argument string.

## Tests (QEMU, PIIX3 IDE, Korean 98 SE)

200 GB disk on the secondary channel with a FAT32 partition starting past
128 GB: `dir`, `type`, a 76 MB copy with `fc /b` by DMA and by PIO; host
check that every write landed inside the partition (nothing written below
128 GB). The Windows disk on the primary channel through kexata: boots,
Performance tab says the system is configured for best performance (no
compatibility mode). 76 CAB files (120 MB) copied from the CD drive through
ATAPI, byte-identical with the ISO.

## Not yet

ATAPI DMA, native-mode PCI IDE and SATA controllers without legacy
channels (AHCI is a separate driver to come), hot-swap, power management
(standby timers), drives over 2 TB.
