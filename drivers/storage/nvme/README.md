# kexnvme.mpd: NVMe SSDs for Windows 98 SE / ME

Own GPLv2 driver: NVMe SSDs (M.2, U.2, PCIe cards; PCI class 01 08 02) as a
SCSI disk for Windows 98's SCSIPORT, like kexata and kexahci. Namespace 1
is target 0. One admin queue and one I/O queue pair of 16 entries; one
command at a time, completion polled (interrupts masked). Reads and writes
up to 128 KB with PRP lists; FLUSH for SYNCHRONIZE CACHE and shutdown.

The register block (BAR 0) is mapped with `MmMapIoSpace`: Windows 98 does
not hand it over, as for AHCI. It has to be below 4 GB (a BIOS puts it
there unless it is told to map PCI above 4 GB).

## Tests (QEMU NVMe, Korean 98 SE)

Installed by the hardware wizard from `INF\OTHER`; the FAT32 SSD is D:; 406
DLLs (filling it) written and compared on the host, `fsck.fat` clean.

## Not yet

4 KB-sector namespaces (only 512-byte blocks), more than one namespace,
interrupts and several commands at once, booting from it (the BIOS's int 13h
until Windows loads; many older BIOSes cannot boot NVMe at all).
