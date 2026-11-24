# RAM disk for the swap file, from RAM Windows does not use (research notes)

Goal (project owner, 2026-11-23): put the swap file on a RAM disk made of the RAM
Windows 98 cannot or does not use: above KEXMEM's limit, and above 4 GB. Target PC:
Dell Latitude E6410 (up to 8 GB). An installer option.

## Reaching the memory: one 4 MB window (tested 2026-11-23)

`drivers/storage/ramdisk/ramtest.c` (KEXRAMT.VXD, test only) at Device_Init:

- takes 8 MB of system linear space (here: `_MapPhysToLinear` of some RAM, so VMM has
  page tables there that we may override) and the page directory (`_MapPhysToLinear`
  of CR3);
- sets CR4.PSE (Windows 98 leaves it clear: CR4 was 0x200) and writes the window's
  page-directory entry as a 4 MB page: `lo & 0xFFC00000 | hi << 13 | P|RW|PS`, with
  physical address bits 32-35 in PDE bits 13-16 (PSE-36), then INVLPG.

QEMU (Westmere CPU, 6 GB, Windows 98 SE with KEXMEM /MAX=2048): four windows (2.25 GB,
4 GB, 4 GB + 4 MB, 5 GB) each kept their own data, and QEMU's monitor shows it at those
physical addresses (`xp 0x100000000` = the tag). Windows then starts normally.

Open: whether the page directory is the same in every memory context (Windows 9x keeps
one directory and copies the private arena's entries on a context switch, as far as we
know; the window is in the system arena). The driver will set the entry right before
each copy with interrupts off, so it works either way; still to measure.

## The disk

Plan: an IOS port driver (VxD in SYSTEM\IOSUBSYS) that creates one fixed-disk DCB, with
an MBR and a FAT32 (or FAT16) volume written into the RAM at start-up, so Windows gives
it a drive letter, and `[386Enh] PagingDrive=` points the swap file at it. Our own code
from the Windows 95/98 DDK documentation (techshelps MSDN archive), no Microsoft headers.
Alternative if IOS port drivers prove too fiddly: a SCSI miniport on a root-enumerated
devnode (like kexahci.mpd, SCSIPORT does the IOS side).

Memory map (done, tested): KEXMEM saves the BIOS's E820 map (up to 32 entries, before
its own filtering) and its limit in its resident part and publishes it in the BIOS data
area's inter-application bytes: 0040:00F0 = 'KX', 00F2 = offset, 00F4 = segment; the
table is 'KXMP', dd limit, dw count, then 20-byte entries. KEXRAMT read it at
Device_Init in QEMU (6 GB, /MAX=2048): RAM 1 MB-3 GB and 4-7 GB, limit 2 GB, so the RAM
disk would get 2-3 GB and 4-7 GB (4 GB).

## KEXRAM.VXD status (2026-11-23, work in progress, not installed by setup)

`drivers/storage/ramdisk/` — `ramcore.c` (chunk selection, FAT16/FAT32 layout; host test
`tests/ramdisk/run.sh`: fsck.fat clean, mtools can write a swap-sized file) and
`kexram.c` (the VxD, SYSTEM.INI `device=KEXRAM.VXD`, init at Sys_VM_Init).

Works in QEMU (6 GB, KEXMEM /MAX=2048): picks 1023 chunks (~4 GB: 2-3 GB and 4-7 GB),
writes the FAT32 volume through the PSE-36 window, IOS_Register succeeds, IOS sends
AEP_DEVICE_INQUIRY (we claim unit 0) and AEP_CONFIG_DCB with its physical DCB (we set
geometry, WRITEABLE, clear MUST_CONFIGURE, insert our calldown), and at appy time
(_SHELL_CallAtAppyTime, CAAFL_RING0) we create the logical DCB and ISP_ASSOCIATE_DCB
gives it Z:. IFSMGR mounts (AEP_CREATE_VRP arrives).

Open problem: on first access, every read the volume gets is IOR_READ of sector 0 with
IOR_xfer_count = 0, flags 0x04080449 (PHYS_SGDS | LOGICAL_START_SECTOR | VERSION_002 |
DOUBLE_BUFFER | BYPASS_VOLTRK | HIGH_PRIORITY), retried forever. The VRP built for the
volume has VRP_max_sgd = 0 (block size 0x200 is right) even though both DCBs have
DCB_max_sg_elements = 17 (offset 0x7B, checked at request time); the demand flags
(small memory or none) and max_xfer_len make no difference. Next: find where IOS takes
VRP_max_sgd from (likely the calldown/DDB, or a field IOS fills in AEP_CONFIG_DCB for a
claimed device that we overwrite), e.g. by comparing with a real disk's VRP/DCB (walk
DCBs with ISP_GET_FIRST_NEXT_DCB as cfu1 does), or set VRP_max_sgd in AEP_CREATE_VRP.
Notes from cfu1-vxd (MIT) that this follows: be your own TSD, appy-time association,
WRITEABLE on both DCBs, logical SG list order {sectors, pointer}.

## Working (2026-11-23, later the same day)

The zero-length reads came from wrong DCB offsets: Microsoft's IOS structures are
byte-packed (IOS reads DCB_max_sg_elements at 0x77 and DCB_apparent_blk_size at 0xDB);
with the packed offsets the volume mounts and reads/writes normally. Also learned:

- The volume can be created during Device_Init (claim the unit in AEP_DEVICE_INQUIRY,
  configure IOS's physical DCB in AEP_CONFIG_DCB, create the logical DCB and
  ISP_ASSOCIATE_DCB right after IOS_Register), early enough for the swap file. Appy
  time stays as a fallback.
- Windows 98 uses one page directory (CR3 was the same for every request), so the
  window's entry is valid in every context. It is now set only for the copy itself.
- Windows sizes the swap file by its own RAM and waits for ever at start-up if the swap
  drive cannot hold it: setup sets MaxPagingFileSize=1048576 (1 GB) and KEXRAM only
  makes Z: when it has at least 1.125 GB.
- Static VxD from the registry (Services\VxD\KEXRAM, StaticVxD=) works: no SYSTEM.INI
  device= line to edit.

Tests (QEMU, Westmere, 6 GB):
- KEXMEM /MAX=2048: 4 GB RAM disk; 535 files (76.5 MB, twice) copied to Z: and
  compared: all identical. Swap file on Z:.
- RAM disk only above 4 GB (3 GB): 540 files copied and compared: all identical.
- Installer `/S /MEMA /RAMD` on a clean Windows: Windows 3072 MB, Z: = 3 GB from RAM
  above 4 GB, WIN386.SWP on Z:.
- Not done: paging under heavy memory pressure. In this VM a 300 MB test program fails
  with a 256 MB Windows even without KEXRAM and with Windows' original VMM, so that test
  says nothing about KEXRAM yet.
