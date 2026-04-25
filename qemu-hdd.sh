#!/bin/sh
#
# qemu-hdd.sh — build Makar, install it to a fresh HDD image with GRUB,
# then boot QEMU directly from the HDD.
#
# This script performs the full installation from the host side — no
# interactive in-kernel installer session is required.
#
# Host tools required (beyond the normal build dependencies):
#   sfdisk    (util-linux)  — write the MBR partition table
#   mkfs.fat  (dosfstools)  — format the FAT32 partition
#   mtools    (mmd, mcopy)  — copy files into the FAT32 image without root
#
# Drive layout produced:
#   sector 0          : GRUB MBR boot code (from boot.img)
#   sectors 1–N       : GRUB core.img (embedding area)
#   LBA 2048 onwards  : FAT32 partition (kernel + GRUB modules + grub.cfg)
#
set -e

# Build the kernel and populate isodir/ with GRUB files (boot.img, core.img,
# *.mod).  iso.sh also builds makar.iso as a side-effect, which is fine.
. ./iso.sh

HDD=hdd.img
HDD_SECTORS=1048576           # 512 MiB total  (512 × 1 048 576 sectors)
PART_START=2048               # first partition at LBA 2048 (1 MiB boundary)
PART_SIZE=$((HDD_SECTORS - PART_START))
GRUB_I386=isodir/boot/grub/i386-pc

# Verify that iso.sh produced all of the files we need.
for _f in "$GRUB_I386/boot.img" "$GRUB_I386/core.img" "isodir/boot/makar.kernel"; do
    if [ ! -f "$_f" ]; then
        echo "Error: required file not found: $_f" >&2
        echo "       (Did grub-mkimage fail during iso.sh?)" >&2
        exit 1
    fi
done

# ---- 1. Create a blank disk image --------------------------------------
echo "Creating HDD image (512 MiB)..."
dd if=/dev/zero of="$HDD" bs=512 count="$HDD_SECTORS" status=none

# ---- 2. Partition table: one FAT32-LBA primary, bootable ---------------
echo "Partitioning..."
printf 'label: dos\nstart=%d, type=c, bootable\n' "$PART_START" | \
    sfdisk "$HDD" > /dev/null 2>&1

# ---- 3. GRUB MBR boot code ---------------------------------------------
echo "Writing GRUB boot code..."

# Patch a private copy of boot.img: write the 64-bit little-endian LBA of
# core.img (= sector 1) at byte offset 0x5C so GRUB knows where to load it.
_boot_tmp=$(mktemp)
cp "$GRUB_I386/boot.img" "$_boot_tmp"
printf '\001\000\000\000\000\000\000\000' | \
    dd of="$_boot_tmp" bs=1 seek=92 count=8 conv=notrunc 2>/dev/null

# Write only the first 446 bytes of boot.img to sector 0, leaving the
# partition table (bytes 446–511) written by sfdisk intact.
dd if="$_boot_tmp" of="$HDD" bs=1 count=446 conv=notrunc 2>/dev/null
rm -f "$_boot_tmp"

# Write core.img to the embedding area (sectors 1..N, before the partition).
_core_sectors=$(( ($(wc -c < "$GRUB_I386/core.img") + 511) / 512 ))
if [ $((_core_sectors + 1)) -gt "$PART_START" ]; then
    echo "Error: core.img ($_core_sectors sectors) is too large for the embedding area." >&2
    exit 1
fi
dd if="$GRUB_I386/core.img" of="$HDD" bs=512 seek=1 conv=notrunc 2>/dev/null

# ---- 4. FAT32 partition -------------------------------------------------
echo "Formatting FAT32 partition..."
_part_img=$(mktemp)
dd if=/dev/zero of="$_part_img" bs=512 count="$PART_SIZE" status=none
mkfs.fat -F 32 -n MAKAR "$_part_img" > /dev/null

echo "Populating FAT32 partition..."
mmd  -i "$_part_img" ::/boot
mmd  -i "$_part_img" ::/boot/grub
mmd  -i "$_part_img" ::/boot/grub/i386-pc

mcopy -i "$_part_img" isodir/boot/makar.kernel ::/boot/makar.kernel

# Copy GRUB modules — silently skip any that are missing.
for _mod in "$GRUB_I386"/*.mod; do
    [ -f "$_mod" ] && \
        mcopy -i "$_part_img" "$_mod" ::/boot/grub/i386-pc/ 2>/dev/null || true
done

# Write grub.cfg: 5-second countdown, then boot Makar automatically.
_cfg_tmp=$(mktemp)
cat > "$_cfg_tmp" << 'GRUBEOF'
set default=0
set timeout=5
set root=(hd0,msdos1)

menuentry "Makar OS" {
	set root=(hd0,msdos1)
	multiboot2 /boot/makar.kernel
	boot
}
GRUBEOF
mcopy -i "$_part_img" "$_cfg_tmp" ::/boot/grub/grub.cfg
rm -f "$_cfg_tmp"

# Splice the populated FAT32 image into the correct offset of the HDD image.
dd if="$_part_img" of="$HDD" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$_part_img"

echo "Installation complete. Booting from HDD..."
qemu-system-$(./src/target-triplet-to-arch.sh $HOST) \
    -drive file="$HDD",format=raw,if=ide,index=0 \
    -boot order=c \
    -serial stdio
