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
- The VRP for D: (ATAPI CD, ESDI_506) read as the DDK layout: demand 0,
  event 0x80, max_sgd 1, max_req_size 0xBC, delta_to_ior 0x64,
  block_size 0x200, fsd_hvol 0, fsd_entry c0049468 (in IOS),
  0x1C = d2ea55c3 (not aligned: a handle, not the DCB), 0x24 a pointer,
  0x28 = d2ea5500 (aligned: the logical DCB, to be confirmed by reading
  its signature). Next session: dump 0x28's target and check the DCB
  layout before building IORs.
- The probe was asked about D: three times (it declined each time) and
  never about a FAT USB stick's drive, which VFAT took first: the order
  in which IFSMgr asks FSDs is still to be pinned down.
- **Test path for steps 2-3**: an exFAT volume inside an MBR partition
  of type 0C (test images only; `stick-exp.img` in the VM folder, filled
  by the host test) gets a drive letter (E:), VFAT declines it, and
  IFSMgr asks KEXFAT (mount request, drive 04). So mounting and reading
  can be developed before the drive-letter layer exists.
- Its VRP: demand 0x00054000, event 0x40, max_sgd 0x11, max_req_size
  0x18C, delta_to_ior 0x134, 0x10 = 0x800, 0x14 = 0x20AEB, fsd_entry
  c0049468, 0x1C = d2ea5893, 0x28 = d2ea57d0. Again 0x1C = 0x28 + 0xC3:
  0x28 is the volume's DCB. The probe now dumps it.
- DCB (from VRP+0x28), matching the DDK's DCB_COMMON: 0x00 physical DCB
  (itself for the CD; the disk's DCB for the stick's partition, so the
  stick volume is a logical DCB), 0x04 expansion length, 0x08 calldown
  pointer, 0x0C next DCB, 0x14/0x15 drive letter / unit (03 = D: for the
  stick, 04 = E: for the CD), 0x18 VRP back pointer, 0x20 device flags,
  0x80+ vendor/product strings and the port driver's name (ESDI_506).
  In the stick's VRP, 0x10 = 0x800 = the partition's first sector (2048).
- Drive letters move: with the stick attached at boot it was D: and the
  CD E:. The FSD must take the drive from the request, never assume it.
- **IOR, as IOS receives it (`IOS_SendCommand` hooked)**: the DDK layout
  holds: 0x00 next, 0x04 func (0 read, 1 write, 0x0D media check),
  0x06 status, 0x08 flags, 0x0C callback, 0x10 start sector (64-bit),
  0x18 count (sectors), 0x1C buffer (or the SGD list), 0x20
  private_client (= the VRP's delta_to_ior), 0x24 IOS private (the IOP),
  0x44 req_vol_handle, 0x48 sgd_lin_phys, 0x4C num_sgds, 0x4D drive
  (0 = A:). Flags seen: 0x400 on every request (version 2), 0x2 with an
  SGD list, 0x8 on volume requests (start sector counted from the
  partition), 0x1 on boot-time reads. EDI = the VRP's 0x1C value.
- **Completion**: IOS calls the IOR's callback with ESI = the IOR, EDI =
  the DCB, EAX = the IOP. The FSD's wait: the callback sets a flag; the
  requesting thread calls IFSMgr_Yield until it is set (no lost wakeup,
  no semaphore service needed).
- VFAT is asked before KEXFAT for FAT volumes (it took the FAT stick and
  KEXFAT never saw it); CDFS after KEXFAT (the CD came to KEXFAT first).

## Stage 1 driver (KEXFAT.VXD) so far

- Registers, gets the mount request for an exFAT volume in a type-0C
  partition (USB stick on kexumass, or a second IDE disk), allocates a
  request packet of VRP max_req_size, fills the IOR at delta_to_ior as
  VFAT does (func 0, flags 0x409, start 0, count 1, callback, 0x20 =
  delta, 0x48 = packet space after the IOR, drive at 0x4D) and sends it
  with IOS_SendCommand (EDI = VRP+0x1C). IOS takes it (fills 0x28,
  calls the callback with ESI = IOR), but completes it with status 0x1C,
  on the stick and on the IDE disk alike. A media check (func 0x0D,
  flags 0x80519, synchronous: no callback) is refused with 0x16.
- Reading of 0x1C: very likely IORS_UNFORMATTED_MEDIA. IOS marks a
  FAT-type partition whose boot sector is not FAT as unformatted and
  refuses I/O through its logical DCB; this is what WINUDF's changelog
  describes ("partitions Windows 9x calls unformatted"). So the type-0C
  test shortcut cannot carry I/O. Next:
  1. read through the physical DCB (VRP+0x28 -> logical DCB, whose
     first field is the physical DCB) with physical sector numbers
     (partition start + sector, without flag 0x8); find the handle form
     IOS_SendCommand wants for a physical DCB (requests IOS itself sends
     to the disk use DCB + 0x83, to a volume DCB + 0xC3);
  2. or go straight to step 4, our own logical DCB for exFAT partitions,
     whose state we control.
- Tried: the same read through the physical DCB (+0x83 handle, physical
  sector numbers): also 0x1C. VRP+0x10 is not the partition start (0x200
  on the IDE disk, 0x800 on the stick): probably a block size or a
  geometry value. Next session: hook IOS_SendCommand from boot and log
  VFAT's mount of a FAT volume on the *IDE* disk (fixed), whose first
  read must succeed, and diff it field by field against ours; and look
  at what IOS does with a type-0C volume it finds unformatted (the
  "unformatted" state may be held in the logical DCB's flags at 0x20,
  which differed between the CD (d021c004) and the stick volume (6013)).
