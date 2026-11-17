#!/bin/sh
# make_iso.sh KEXSETUP.IMG CSMWRAP.EFI OUT.ISO
# Hybrid image for CD and USB stick (dd it, or write it with the coming Windows tool):
#  - BIOS: isolinux -> MEMDISK -> KEXSETUP.IMG as a RAM hard disk (syslinux, GPLv2)
#  - UEFI: \EFI\BOOT\BOOTX64.EFI = CSMWrap (LGPL-2.1), which provides a legacy BIOS and
#    boots the same medium the BIOS way.
# The MBR (isohdpfx) carries the disk signature KXIS so KEXPREP never offers the stick.
set -e
IMG=$1 CSM=$2 OUT=$3
[ -n "$OUT" ] || { echo "usage: $0 KEXSETUP.IMG CSMWRAP.EFI OUT.ISO"; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/iso/isolinux" "$W/iso/EFI/BOOT"
cp /usr/lib/ISOLINUX/isolinux.bin /usr/lib/syslinux/modules/bios/ldlinux.c32 \
   /usr/lib/syslinux/memdisk "$W/iso/isolinux/"
cp "$IMG" "$W/iso/KEXSETUP.IMG"
C=$(( $(stat -c %s "$IMG") / 512 / 16065 ))
cat > "$W/iso/isolinux/isolinux.cfg" <<EOF
DEFAULT kex
PROMPT 0
TIMEOUT 0
LABEL kex
  KERNEL memdisk
  INITRD /KEXSETUP.IMG
  APPEND harddisk c=$C h=255 s=63
EOF
cp "$CSM" "$W/iso/EFI/BOOT/BOOTX64.EFI"
# El Torito EFI image (FAT) holding the same loader
SZ=$(( ( $(stat -c %s "$CSM") / 1024 + 512 ) ))
mkfs.fat -C "$W/iso/efi.img" $SZ >/dev/null
MTOOLS_SKIP_CHECK=1 mmd -i "$W/iso/efi.img" ::/EFI ::/EFI/BOOT
MTOOLS_SKIP_CHECK=1 mcopy -i "$W/iso/efi.img" "$CSM" ::/EFI/BOOT/BOOTX64.EFI
# isohybrid MBR with our signature at 0x1B8
head -c 432 /usr/lib/ISOLINUX/isohdpfx.bin > "$W/mbr.bin"
xorriso -as mkisofs -quiet -o "$OUT" -V KEXSETUP -J -r \
  -isohybrid-mbr "$W/mbr.bin" \
  -b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table \
  -eltorito-alt-boot -e efi.img -no-emul-boot -isohybrid-gpt-basdat \
  "$W/iso"
printf 'KXIS' | dd of="$OUT" bs=1 seek=440 conv=notrunc status=none
ls -l "$OUT"
