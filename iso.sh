#!/bin/sh
# iso.sh — package the staged sysroot into bootable ISO(s).
#
# Always produces makar.iso (interactive menu, default entry 0 = Makar OS).
# When TEST_ISO=1 also produces makar-test.iso, whose grub.cfg has a single
# entry that auto-boots with the multiboot2 `test_mode` cmdline arg.  Both
# ISOs share the same makar.kernel binary so the kernel only compiles once.
#
# Env vars:
#   TEST_ISO=1    Also emit makar-test.iso
#   KERNEL_ARGS   Extra cmdline appended to the interactive menuentry
#                 (rarely needed; test mode uses its own ISO)
set -e
. ./build.sh

mkdir -p isodir/boot/grub/i386-pc isodir/apps isodir/src isodir/docs

cp sysroot/boot/makar.kernel isodir/boot/makar.kernel

# Copy source tree and docs onto the ISO so they're readable via VIX.
cp -r src/. isodir/src/
cp -r docs/. isodir/docs/

# Locate the host GRUB i386-pc directory and stage the modules / boot.img /
# core.img that the in-kernel installer expects to find on the ISO.  This
# block is independent of grub.cfg content, so it runs once before both
# ISO variants are packaged.
GRUB_DIR=""
for _d in /usr/lib/grub/i386-pc /usr/share/grub/i386-pc; do
    if [ -d "$_d" ]; then
        GRUB_DIR="$_d"
        break
    fi
done

if [ -z "$GRUB_DIR" ]; then
    echo "Warning: GRUB i386-pc directory not found." >&2
    echo "The 'install' command will fail to read GRUB files from /boot/grub/i386-pc/." >&2
else
    if [ -f "$GRUB_DIR/boot.img" ]; then
        cp "$GRUB_DIR/boot.img" isodir/boot/grub/i386-pc/boot.img
    else
        echo "Warning: boot.img not found in $GRUB_DIR." >&2
    fi

    # core.img: standalone BIOS-bootable core image the installer writes to
    # sectors 1..N of the target HDD.  Prefix /boot/grub tells GRUB where to
    # find modules on the FAT32 partition; the embedded -c config runs a
    # search across all partitions for /boot/grub/grub.cfg so the FAT32
    # partition (hd0,msdos1) wins over the raw disk (hd0).
    _embed_cfg=$(mktemp)
    trap 'rm -f "$_embed_cfg"' EXIT
    printf 'search --no-floppy --file --set=root /boot/grub/grub.cfg\n' \
        > "$_embed_cfg"
    grub-mkimage \
        -O i386-pc \
        -o isodir/boot/grub/i386-pc/core.img \
        -p /boot/grub \
        -c "$_embed_cfg" \
        biosdisk part_msdos fat search search_fs_file normal multiboot2 linux \
        || echo "Warning: grub-mkimage failed; core.img will be missing." >&2
    rm -f "$_embed_cfg"
    trap - EXIT

    for _mod in normal part_msdos fat multiboot2 linux; do
        if [ -f "$GRUB_DIR/${_mod}.mod" ]; then
            cp "$GRUB_DIR/${_mod}.mod" "isodir/boot/grub/i386-pc/${_mod}.mod"
        fi
    done
fi

# ── Interactive ISO ──────────────────────────────────────────────────────────
# Shipped menu: default = Makar OS, 5s timeout, with a fallback that exits to
# the next bootable device.  KERNEL_ARGS appends to the main entry only.
cat > isodir/boot/grub/grub.cfg << EOF
set default=0
set timeout=5

menuentry "Makar OS" {
	multiboot2 /boot/makar.kernel${KERNEL_ARGS:+ $KERNEL_ARGS}
}

menuentry "Next available device" {
	exit
}
EOF

grub-mkrescue -o makar.iso isodir

# ── Test ISO (CI) ────────────────────────────────────────────────────────────
# Single entry, zero timeout: QEMU boots straight into ktest_run_all().
if [ "${TEST_ISO:-0}" = "1" ]; then
    cat > isodir/boot/grub/grub.cfg << 'EOF'
set default=0
set timeout=0

menuentry "Makar OS (test_mode)" {
	multiboot2 /boot/makar.kernel test_mode
}
EOF
    grub-mkrescue -o makar-test.iso isodir

    # Restore the interactive grub.cfg in the staged isodir so anyone
    # inspecting the staging dir doesn't see the test variant.
    cat > isodir/boot/grub/grub.cfg << EOF
set default=0
set timeout=5

menuentry "Makar OS" {
	multiboot2 /boot/makar.kernel${KERNEL_ARGS:+ $KERNEL_ARGS}
}

menuentry "Next available device" {
	exit
}
EOF
fi
