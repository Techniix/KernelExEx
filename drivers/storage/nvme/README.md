# kexnvme.mpd: NVMe SSDs for Windows 98 SE / ME

Own GPLv2 driver: NVMe SSDs (M.2, U.2, PCIe cards; PCI class 01 08 02) as a
SCSI disk for Windows 98's SCSIPORT, like kexata and kexahci. Namespace 1
is target 0. Reads and writes up to 128 KB with PRP lists; FLUSH for
SYNCHRONIZE CACHE and shutdown.

- I/O queue 1 takes up to 8 reads and writes at once; they complete in the
  interrupt (pin-based, vector 0) once Windows turns it on. Same ordering
  rules as kexahci: overlapping requests with a write among them wait, a
  flush waits for the queue to drain and holds everything behind it.
- I/O queue 2 is waited on: during startup, without an interrupt, and when
  queue 1 is full.
- A time-out resets the controller and sets it up again.
- `AdapterSettings`: `POLL` (no interrupts), `NOQUEUE` (one command at a
  time), `TAKEOVER` (see below).
- An SSD the BIOS has already switched on (it serves it through int 13h,
  perhaps as the disk Windows starts from) is left to the BIOS: Windows
  reaches it through int 13h in compatibility mode, slower but safe. Taking
  such a controller over resets it under the BIOS, and Windows still calls
  the BIOS for its startup disk while it starts: in testing that gave a
  protection error and then a damaged FAT. `TAKEOVER` uses the SSD anyway;
  only for an SSD that holds data, never for the one Windows starts from.
- 512-byte blocks only: Windows 98's disk stack knows no other sector size.
  On an SSD formatted with 4 KB blocks the driver writes to the log whether
  the drive also offers a 512-byte format (switching formats erases the
  drive, so the driver never does it). Showing 512-byte sectors on a 4 KB
  namespace by reading, merging and writing back partial blocks was tried
  and corrupted data in QEMU, cause not found yet. A BIOS that presents a
  4 KB-sector drive through int 13h also stops Windows 98's real-mode
  startup (seen with SeaBIOS: the system restarts in a loop).

The register block (BAR 0) is mapped with `MmMapIoSpace`: Windows 98 does
not hand it over, as for AHCI. It has to be below 4 GB (a BIOS puts it
there unless it is told to map PCI above 4 GB).

## Tests (QEMU NVMe, Korean 98 SE)

Installed by the hardware wizard from `INF\OTHER`; the FAT32 SSD is D:; 406
DLLs (filling it) written and compared on the host, `fsck.fat` clean.

Booting from the SSD (SeaBIOS): the driver leaves it to the BIOS, Windows
starts in compatibility mode, 16 files written to C: identical, `fsck.fat`
clean. (Before this rule: a protection error during startup and a damaged
FAT.) A data SSD the BIOS also switched on, with `TAKEOVER`: 406 files
identical, up to 4 commands in flight. (Without `TAKEOVER` SeaBIOS's own
NVMe code fails Windows 98's real-mode transfers: it refuses buffers that
are not 4-byte aligned; real BIOSes should not.)

With interrupts and queuing: the same test, 406 files identical, up to 3
commands in flight; again with the waited-on path forced (`KEX_TEST_POLL`
build), 406 files identical. A 4 KB-block SSD is refused with the message
and Windows starts normally (QEMU with a SeaBIOS built without NVMe
support, since the stock one hands DOS the 4 KB drive).

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

4 KB-sector namespaces, more than one namespace, starting Windows from an
NVMe SSD at full speed (sharing the controller with the BIOS during startup:
our own I/O queues beside the BIOS's), telling a data SSD from the startup
SSD without `TAKEOVER` (the BIOS's int 13h
until Windows loads; many older BIOSes cannot boot NVMe at all).
