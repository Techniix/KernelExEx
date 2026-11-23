#!/bin/sh
# tests/kexpatch/run.sh VMM32.VXD: the VxD container code under ASan/UBSan on a real
# VMM32.VXD (from an installed Windows 98: C:\WINDOWS\SYSTEM\VMM32.VXD; not shipped).
set -e
[ $# = 1 ] || { echo "usage: $0 VMM32.VXD"; exit 1; }
cd "$(dirname "$0")"
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -o /tmp/vftest vftest.c ../../tools/kexpatch/vxdfile.c
/tmp/vftest "$1"
echo "kexpatch: ok"
