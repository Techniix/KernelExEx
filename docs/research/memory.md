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

## Ways forward

1. Find the per-64 MB page's owner (VMM32 loader, real mode) and avoid it, so the
   ranges merge (no change to VMM).
2. Patch VMM: a bigger range table (relocate it; the LE fixups list every reference),
   and fix the one-past-the-end return. Tools: `vmmx` (W4 -> W3 -> VMM.VXD, from
   patcher9x's MIT code).
