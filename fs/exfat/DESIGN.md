# exFAT for Windows 98 SE / ME: design

Goal: SD cards, USB sticks and disks formatted exFAT (the default for cards
over 32 GB and for most sticks sold today) get a drive letter and work like
any FAT drive: Explorer, programs, long names, reading and writing.

## Layers

1. **`fs/exfat/exfat.c`**: the file system itself, portable C, no OS
   calls (done: read/write, tested against `mkfs.exfat` images and
   `fsck.exfat`, `tests/exfat/run.sh`).
2. **`fs/exfat/win98/`**: `KEXFAT.VXD`, a file system driver (FSD) for
   Windows 98's IFS Manager, the same kind of driver as VFAT and CDFS:
   - registers with `IFSMgr_RegisterMount` at Device_Init (static VxD,
     loaded from the registry's `Services\VxD` key like VKrnlEx);
   - answers IFSMgr's mount request for a volume whose boot sector says
     `EXFAT   `, and declines every other volume so VFAT keeps its own;
   - reads and writes sectors through IOS, with I/O requests (IOR) sent to
     the volume's DCB, as VFAT does: works for every IOS disk, i.e. IDE
     (ESDI_506 or our kexata), SCSI, and USB through kexumass + NTMAP;
   - fills IFSMgr's volume function table (open, find first/next, file
     attributes, delete, rename, directory create/remove, disk info,
     flush) and handle function table (read, write, seek, close, commit,
     file times, handle info) from the core;
   - names: IFSMgr gives Unicode paths (`ParsedPath`), exactly what the
     core takes; short 8.3 names, which DOS programs need, are made with
     IFSMgr's `CreateBasis` / `AppendBasisTail` and are not stored (exFAT
     has none): a directory listing gives the same short name every time
     for the same long names, as CDFS does for Joliet.
3. **Drive letters for exFAT partitions.** Windows 98's IOS gives a letter
   only to partitions with FAT type bytes (01, 04, 06, 0B, 0C, 0E). exFAT
   partitions are type 07 (shared with NTFS), so they get no logical
   drive and IFSMgr never asks any FSD to mount them. Options, in order of
   preference:
   - a small IOS layer driver (a VSD in the TSD position) that adds a
     logical DCB for type-07 partitions whose boot sector says EXFAT
     (the way IOS's DISKTSD adds them for FAT); then IFSMgr asks the FSDs,
     and KEXFAT mounts it;
   - sticks and cards formatted without a partition table ("superfloppy")
     already get a letter as a removable drive, so only the FSD is needed
     for them.
   The type byte on the user's disk is never changed.

## Order of work

1. FSD skeleton: register, log every mount request (debug console in the
   test VM), decline them all. Learn exactly what IOS hands over for a USB
   stick with an exFAT superfloppy and with a type-07 partition.
2. Mount + read-only: disk info, find first/next, open, read, close.
   Tested with an image made on Linux (files written by the core's host
   test) in QEMU on kexumass.
3. Writing: write, create, delete, rename, directories, flush on
   unmount/eject. `fsck.exfat` on the image afterwards.
4. The IOS layer for type-07 partitions.
5. Installer component ("exFAT drives"), README row.

## Risks

- IFS structure layouts (ioreq, volfunc/hndlfunc tables, ParsedPath) and
  IFSMgr service numbers are written from the documented interface and
  checked one by one in the VM; nothing is copied from other drivers
  (WINUDF, which shows the same approach works, is GPL-3.0 and cannot be
  used here).
- Long operations must not run at interrupt time and must not block IOS:
  sector I/O through IOS is synchronous from the FSD's point of view
  (`IOS_SendCommand` with a completion event).
- Memory: the allocation bitmap is kept in memory (128 KB for a 128 GB
  card with 128 KB clusters); fine for Windows 98's locked heap.

## Findings (KEXFATP.VXD probe, QEMU, 98 SE)

- A static VxD needs VKrnlEx's DDB constants ('Prev' = 0x50726576,
  reserved 'Rsv1..3') and le_link.py's default (dynamic) module flags;
  with `--static` and byte-swapped constants VMM never loaded it (no line
  in BOOTLOG.TXT).
- `IFSMgr_RegisterMount(fn, 0x22, NORMAL_FSD)` returns provider id 2.
  IFSMgr asks FSDs only for volumes that have a drive letter, and only
  when one is first used (Explorer opening D: produced the first request).
- The mount ioreq: ir_flags 0 (IR_FSD_MOUNT), ir_aux1 (0x10) = the VRP,
  ir_aux2 (0x28) = drive number (3 = D:), ir_rh (0x1C) and ir_fh (0x20)
  = IFSMgr's structures, "IORE" signature at 0x70.
- **An exFAT USB stick without a partition table gets no drive letter**:
  IOS's disk TSD reads its sector 0 as an MBR (it ends in 55 AA) with no
  partitions. So the drive-letter layer (step 4) is needed for sticks as
  well as for type-07 partitions; it comes before mounting can be tested.
