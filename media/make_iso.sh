#!/bin/sh
# make_iso.sh KEXSETUP.IMG REBIOS.EFI CSMWRAP.EFI OUT.ISO
# Hybrid image for CD and USB stick (dd it, or write it with the coming Windows tool):
#  - BIOS: isolinux -> MEMDISK -> KEXSETUP.IMG as a RAM hard disk (syslinux, GPLv2)
#  - UEFI: \EFI\BOOT\BOOTX64.EFI = ReBIOS (GPLv2; SeaBIOS inside a thin hypervisor:
#    needs VT-x with EPT or AMD-V, and an IOMMU, switched on in the firmware setup).
#    \EFI\CSMWRAP\CSMWRAP.EFI = CSMWrap (LGPL-2.1, deprecated by its authors) for
#    machines without those: start it from the firmware's "boot from file" menu.
#    Both provide a legacy BIOS and boot this stick the BIOS way.
# The MBR (isohdpfx) carries the disk signature KXIS so KEXPREP never offers the stick.
set -e
IMG=$1 REB=$2 CSM=$3 OUT=$4
[ -n "$OUT" ] || { echo "usage: $0 KEXSETUP.IMG REBIOS.EFI CSMWRAP.EFI OUT.ISO"; exit 1; }
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
mkdir -p "$W/iso/EFI/CSMWRAP"
cp "$REB" "$W/iso/EFI/BOOT/BOOTX64.EFI"
cp "$CSM" "$W/iso/EFI/CSMWRAP/CSMWRAP.EFI"
# El Torito EFI image (FAT) holding the same loader
SZ=$(( ( $(stat -c %s "$REB") + $(stat -c %s "$CSM") ) / 1024 + 1024 ))
mkfs.fat -C "$W/iso/efi.img" $SZ >/dev/null
MTOOLS_SKIP_CHECK=1 mmd -i "$W/iso/efi.img" ::/EFI ::/EFI/BOOT ::/EFI/CSMWRAP
MTOOLS_SKIP_CHECK=1 mcopy -i "$W/iso/efi.img" "$REB" ::/EFI/BOOT/BOOTX64.EFI
MTOOLS_SKIP_CHECK=1 mcopy -i "$W/iso/efi.img" "$CSM" ::/EFI/CSMWRAP/CSMWRAP.EFI
# isohybrid MBR with our signature at 0x1B8
head -c 432 /usr/lib/ISOLINUX/isohdpfx.bin > "$W/mbr.bin"
xorriso -as mkisofs -quiet -o "$OUT" -V KEXSETUP -J -r \
  -isohybrid-mbr "$W/mbr.bin" \
  -b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table \
  -eltorito-alt-boot -e efi.img -no-emul-boot -isohybrid-gpt-basdat \
  "$W/iso"
printf 'KXIS' | dd of="$OUT" bs=1 seek=440 conv=notrunc status=none
ls -l "$OUT"
