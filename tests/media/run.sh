#!/bin/sh
# tests/media/run.sh W98.ISO PAYLOAD: build the install stick on the host (mkimage.c under
# ASan/UBSan), then check it: the ISO holds KEXSETUP.IMG with MEMDISK's geometry, the image's
# FAT16 is clean and holds IO.SYS first, our stage and the CD's WIN98 folder.
# PAYLOAD: media/pack_media.py output (or a packed kexmedia.exe). The ISO is your own CD.
set -e
[ $# = 2 ] || { sed -n 2,5p "$0"; exit 1; }
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -o "$T/mkhost" mkhost.c ../../media/win/mkimage.c
"$T/mkhost" "$1" "$2" "$T/stick.img" 2>&1 | tail -1
osirrox -indev "$T/stick.img" -extract /KEXSETUP.IMG "$T/K.IMG" -extract /isolinux/isolinux.cfg "$T/cfg" >/dev/null 2>&1
C=$(sed -n 's/.*c=\([0-9]*\) .*/\1/p' "$T/cfg")
[ "$C" -eq $(( $(stat -c %s "$T/K.IMG") / 512 / 16065 )) ] || { echo "FAIL: geometry"; exit 1; }
dd if="$T/K.IMG" of="$T/part" bs=512 skip=63 status=none
fsck.fat -n "$T/part" >/dev/null || { echo "FAIL: fsck"; exit 1; }
export MTOOLS_SKIP_CHECK=1
first=$(mdir -a -b -i "$T/part" ::/ | head -1)
[ "$first" = "::/IO.SYS" ] || { echo "FAIL: IO.SYS is not first ($first)"; exit 1; }
for f in ::/MSDOS.SYS ::/CONFIG.SYS ::/AUTOEXEC.BAT ::/KEXMEM.SYS ::/KEXPREP.EXE ::/KEXINST/T_CONFIG.SYS \
         ::/KEXINST/MSBATCH.INF ::/WIN98/BASE4.CAB; do
  mdir -a -i "$T/part" "$f" >/dev/null 2>&1 || { echo "FAIL: $f missing"; exit 1; }
done
mtype -i "$T/part" ::/CONFIG.SYS | head -1 | grep -q KEXMEM || { echo "FAIL: CONFIG.SYS"; exit 1; }
echo "media: stick image ok (c=$C)"
