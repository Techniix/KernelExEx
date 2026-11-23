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

Memory map: KEXMEM saves the BIOS E820 map (before its own filtering) and its limit
where the VxD can find it (BIOS data area 0040:00F0, inter-application area, signature
plus segment), so the RAM disk takes exactly the RAM KEXMEM hid plus all RAM above 4 GB.
