#!/bin/sh
# tests/ramdisk/run.sh: RAM disk core (chunk selection, FAT volume) on the build host.
# Checks the volumes with fsck.fat and writes a file onto them with mtools.
set -e
cd "$(dirname "$0")"
gcc -std=c99 -Wall -Wextra -Werror -g -fsanitize=address,undefined -o /tmp/rdtest ramtest_host.c ../../drivers/storage/ramdisk/ramcore.c
for secs in 65536 524288 1048576 4194304; do
	/tmp/rdtest /tmp/rd.img $secs
	fsck.fat -n /tmp/rd.img
	dd if=/dev/urandom of=/tmp/rd.bin bs=1M count=3 2>/dev/null
	mcopy -i /tmp/rd.img /tmp/rd.bin ::/WIN386.SWP
	mdir -i /tmp/rd.img :: | grep -i -E "volume|WIN386"
	fsck.fat -n /tmp/rd.img >/dev/null
done
rm -f /tmp/rd.img /tmp/rd.bin
echo "ramdisk: ok"
