#!/bin/sh
# build_usbmaker.sh BUILD_CORE KEXPREP.EXE KEXSETUP.EXE REBIOS.EFI CSMWRAP.EFI OUT.EXE
# The one-file USB stick maker for users: kexmedia.exe (from build_core.py) with its
# payload appended (our DOS stage, the base ISO, our MBR). Needs: python3, xorriso,
# mtools, dosfstools, syslinux-common, isolinux. GPLv2.
set -e
[ $# = 6 ] || { sed -n 2,5p "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
python3 "$HERE/stage_extra.py" "$1" "$2" "$3" "$W/stage" >/dev/null
python3 "$HERE/pack_media.py" "$W/stage" "$4" "$5" "$6" "$1/kexmedia.exe" 2>&1 | grep -v "^xorriso\|^$"
