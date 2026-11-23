# Windows 98 with more than 1 GB of RAM: research notes (clean-room)

Our own analysis of Windows 98 SE's VMM (from the user's own CD, Korean OEM edition),
in QEMU with its gdb stub. PATCHMEM (R. Loew) is not used or read: the user considers it
proprietary. patcher9x (JHRobotics, MIT) is used only for its W3/W4 container code
(`pe/pew.c`, `pe/ds_*.c`), not for its PATCHMEM-derived parts (`rloew/`).

## What happens (2026-11-23)

QEMU, 2 GB guest, Westmere CPU, Windows 98 SE, KEXMEM `/MAX=n` or `MaxPhysPage`:

| Windows sees | Result |
|---|---|
| 1024 MB | starts (9/9) |
| 1280, 1536, 1792 MB | Normal mode: reset a few seconds after WIN.COM (triple fault) |
| 2 GB (Safe Mode: CONFIG.SYS skipped, so no KEXMEM) | "Insufficient memory to initialize Windows" |

VMM's memory-manager start-up (in its init code; VMM loads at C0001000):

- A builder (at C036384B in this build) walks the loader's page tables and collects the
  ranges of present, free physical pages, then copies them, merged where contiguous,
  into a fixed table in VMM's data: 16 entries of (first page, last page) at C0016F08,
  count at C0016058. It stops at 16 entries (`cmp ecx, C0016F88`) and returns a pointer
  to the last entry, which is one past the end when the table is full.
- The ranges are about 64 MB each: one page is missing every 0x4000 pages (64 MB), so the
  ranges do not merge. 16 x 64 MB = 1 GB: the 1 GB limit.
- With 2 GB (no limit) the table fills, the caller (C0363D51) reads the "last page" from
  past the end (a pointer), sizes its page database (13 bytes per page) from it,
  `_PageReserve` fails, MMInit returns 0, and VMM calls `Fatal_Memory_Error`
  (service 191, C00019D4): "Insufficient memory to initialize Windows".
- With 1280 MB the table holds the ranges but start-up later faults at C00140CB
  (a hash-chain walk reaching NULL, CR2=4), then double fault and reset.

KEXXMS.SYS (our XMS filter that rounds "largest free block" to whole pages) removed the
reset at 1280 MB (Windows then hung before the desktop) but the 64 MB ranges remained,
so the missing page is not from block misalignment. Still to find: what the missing
page per 64 MB is (probably the loader's own use of each XMS block).

## Update: the missing page and a first patch

- The ranges come from the loader's list of XMS blocks (init block +0Ch): one entry per
  block of at most 64 MB (XMS 2.0 sizes are 16-bit KB). The page between two blocks is
  left out by the loader's rounding (it is zero and belongs to Windows' own blocks).
  Rounding/aligning the XMS blocks (KEXXMS.SYS experiments) does not change that.
- **Merge patch** (tested live with gdb, writing VMM's init code before it runs): in the
  range builder's merge test (C03639A2 in this build), merge when the next range starts
  1 *or 2* pages after the previous one ends:
  `8B 41 04 2B 03 83 F8 FF 75 08` -> `8B 03 2B 41 04 48 D1 E8 75 08`
  (mov eax,[ebx]; sub eax,[ecx+4]; dec eax; shr eax,1; jnz). Same length, same target.
  The table then holds 1-2 ranges. Results (2 GB guest, KEXMEM /MAX):
  1088, 1152, 1216, 1248 MB reach the desktop; 1272, 1280, 1536 MB fail.
- **Second limit at ~1260 MB:** 0x4EC4E pages x 13 bytes (VMM's page database) = 4 MB.
  Above it, a page-table record (in the region VMM reserves after the database) is
  found flagged but missing from VMM's hash of such records (lookup at C00140A0 walks to
  NULL: page fault at C00140CB, CR2=4, then double fault). The records are set up in
  C0364220..C03642E5. Not solved yet.

## Ways forward

1. Find the per-64 MB page's owner (VMM32 loader, real mode) and avoid it, so the
   ranges merge (no change to VMM).
2. Patch VMM: a bigger range table (relocate it; the LE fixups list every reference),
   and fix the one-past-the-end return. Tools: `vmmx` (W4 -> W3 -> VMM.VXD, from
   patcher9x's MIT code).

## Update: past 2 GB, at parity with PATCHMEM (2026-11-23)

PATCHMEM 7.2 (Rudolph R. Loew; source published by his family) was run in QEMU as a
reference: Korean 98 SE reaches the desktop at 3072 MB with it. Its output was compared
byte for byte with ours. What we learned:

- PATCHMEM writes a standalone SYSTEM\VMM32\VMM.VXD (and VCACHE.VXD), patches the
  real-mode loader in VMM32.VXD to take XMS 3.0 blocks (one large block instead of
  64 MB ones), and patches VMM and VCACHE. We patch VMM and VCACHE inside VMM32.VXD
  (W3) and keep our range merge instead of the loader patch.
- **VMM's page map is not in object order**: the init object (ICOD) starts at file
  page 0x23, not 0x19. Our first port assumed identity and put the pool hook into the
  middle of the start-up page database; vxdfile.c now maps pages through the page map.
- The start-up page database (13 bytes per page) sits in ICOD at 0x9430, directly
  before the record pool's byte array (0xFEF0, 0x52D0 bytes). Raising the start-up page
  cap 0x834 -> 0xE90 only works together with moving that array (PATCHMEM's pool patch):
  this is why the cap raise alone failed before.
- With 64 MB XMS blocks, HIMEM's default 32 handles run out at ~2.75 GB (Windows then
  sees 2824 MB): the installer sets /NUMHANDLES=128 (PATCHMEM's manual gives the same
  advice for the user to do by hand).

Results (QEMU, Westmere, full set + merge): 2048, 3072 and 3560 MB (all a 32-bit PC
maps below 4 GB) reach the desktop and report the full amount; also through the
installer (/S /MEMA). Not yet tested: long runs under memory load.
