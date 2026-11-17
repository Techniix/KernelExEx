#!/bin/sh
# build_vesa.sh OUTDIR: builds the VESA display driver (vesamini.drv + vesamini.vxd) from
# JHRobotics' vmdisp9x (MIT; based on Michal Necasek's driver) with Open Watcom 2.0.
# The driver links Open Watcom's runtime (OWPL), so it ships as its own MIT component,
# not as part of the GPL KernelExEx code. Needs WATCOM set and git.
set -e
HERE=$(dirname "$(realpath "$0")")
OUT=$(realpath -m "$1")
REV=84c3ce8   # vmdisp9x commit tested with KernelExEx
SRC=${VMDISP9X_SRC:-$OUT/src}
[ -d "$SRC/.git" ] || git clone -q https://github.com/JHRobotics/vmdisp9x "$SRC"
cd "$SRC"
git checkout -q "$REV"
git submodule update --init -q
PATH=$WATCOM/binl64:$PATH INCLUDE=$WATCOM/h wmake -h vesamini.drv vesamini.vxd
mkdir -p "$OUT"
cp vesamini.drv vesamini.vxd LICENSE "$OUT/"
cp "$HERE/kexvesa.inf" "$OUT/"
echo "$OUT: vesamini.drv vesamini.vxd kexvesa.inf"
