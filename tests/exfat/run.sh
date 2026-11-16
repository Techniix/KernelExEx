#!/bin/sh
# Host tests for fs/exfat: mkfs.exfat images at several cluster sizes, our
# writes, then fsck.exfat must find nothing wrong.
set -e
out=${1:-/tmp/exfat-test}
mkdir -p "$out"
here=$(dirname "$0")
cc ${CFLAGS_EXTRA} -O2 -std=gnu99 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -o "$out/test_exfat" "$here/test_exfat.c" "$here/../../fs/exfat/exfat.c"
for cl in 4K 32K 128K; do
	img="$out/t-$cl.img"
	rm -f "$img"; truncate -s 96M "$img"
	mkfs.exfat -q -c "$cl" -L KEXTEST "$img"
	"$out/test_exfat" "$img" 7
	fsck.exfat -n "$img"
done
# bitmap packed into the FAT area, and a 1 MB-aligned cluster heap
img="$out/t-pack.img"
rm -f "$img"; truncate -s 200M "$img"
mkfs.exfat -q --pack-bitmap -b 1M -c 64K "$img"
"$out/test_exfat" "$img" 9
fsck.exfat -n "$img"
echo "exfat: all images pass"
