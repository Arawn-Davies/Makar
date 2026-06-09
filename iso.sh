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

# ── Build TCC (tcc.elf + sysroot) ────────────────────────────────────────────
# build-tcc.sh compiles vendor/tinycc into tcc.elf and stages TCC's runtime
# (libtcc1.a, built-in headers, CRT stubs) into isodir/usr/lib/.
# Runs after build.sh so crt0.o and libc.a are available.
bash build-tcc.sh

mkdir -p isodir/boot/grub/i386-pc isodir/apps isodir/src isodir/docs isodir/limine isodir/etc

cp sysroot/boot/makar.kernel isodir/boot/makar.kernel

# Default /etc/hostname.  sys_gethostname() reads this; if absent the kernel
# falls back to "makar".  Operators can edit this file at any time (write
# /etc/hostname mybox  from the rescue shell) to change the prompt.
echo "makar" > isodir/etc/hostname

# Default /etc/shadow for the live CD: one account `user` with password `user`.
# Format: name:$mh$<16-hex-salt>$<16-hex-microhash(salt_hex++password)>:::::::
# (microhash is the kernel's auth hash; this entry is precomputed against the
#  canonical algorithm so `login` accepts user/user on a live boot, and the GUI
#  Logout / shell `logout` flows return to a working login prompt.)
{
  echo 'root:$mh$fedcba9876543210$b0dc9b7e4dd3cae6:::::::'
  echo 'user:$mh$0123456789abcdef$b9cdde36b9972a60:::::::'
} > isodir/etc/shadow

# Copy source tree and docs onto the ISO so they're readable via VIX.
cp -r src/. isodir/src/
cp -r docs/. isodir/docs/

# Kernel headers re-exposed at /usr/include/kernel-build/ so the in-OS
# rebuild script (rebuild-kernel.sh, run from kernel-sh) can pass
# `-I/usr/include/kernel-build` and have `#include <kernel/foo.h>` resolve.
# Keeps it separate from /usr/include/ (which carries the userspace libc).
mkdir -p isodir/usr/include/kernel-build
cp -r src/kernel/include/. isodir/usr/include/kernel-build/

# Also stage src/libc/include/ on top: the kernel-side libc headers
# (string.h, stdio.h, ...) drag in <stddef.h> for size_t/NULL, which
# the libc *.c sources in /src/libc/string and /src/libc/stdio need.
# The userspace /usr/include/string.h deliberately avoids stddef and
# uses a private string_size_t typedef instead -- great for the
# ring-3 apps, wrong for the freestanding libc sources.  Without this
# overlay TCC would pick up the userspace string.h and fail with
# "'size_t' undeclared" / "'NULL' undeclared".
cp -r src/libc/include/. isodir/usr/include/kernel-build/

# Stage vendor/tinycc/ source onto the ISO at /src/tinycc/ so the in-OS TCC
# can rebuild itself (the v1.0 self-host flex).  build-tcc.sh has already
# applied the Makar-flavoured patches above, so the staged copy matches the
# host-built tcc.elf exactly.  We skip the host build artefacts (*.o, *.log,
# tcc.elf) -- they'd just be rebuilt anyway.
if [ -d vendor/tinycc ]; then
    mkdir -p isodir/src/tinycc
    (cd vendor/tinycc && find . -type f \
       \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.def' \) \
       -exec cp --parents {} ../../isodir/src/tinycc/ \;)
fi

# Vendored limine BIOS stage (v12.3.0).  The in-kernel installer reads this
# off the CD and deploys it to the target HDD (boot sector -> MBR, stage2 ->
# post-MBR gap, the file itself -> /limine on the rootfs).  The host build
# itself still boots via GRUB (grub-mkrescue, below); only the runtime
# installer uses limine.  See vendor/limine/VERSION.
if [ -f vendor/limine/limine-bios.sys ] && [ -f vendor/limine/limine-hdd.bin ]; then
    cp vendor/limine/limine-bios.sys isodir/limine/limine-bios.sys
    cp vendor/limine/limine-hdd.bin  isodir/limine/limine-hdd.bin
else
    echo "Warning: vendor/limine/{limine-bios.sys,limine-hdd.bin} missing; 'install' will fail." >&2
fi

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

# RES=<mode> from run.sh arrives as MAKAR_VMODE; bake it as vmode= onto the
# standard boot entries (incl. the default GUI desktop, which otherwise carries
# no cmdline args) so a chosen resolution applies without hand-editing GRUB.
# Empty (the default) leaves the kernel's built-in 720p.  Not applied to the
# resolution submenu (those set their own vmode) nor to sysadmin mode (text-only).
_vmode="${MAKAR_VMODE:+ vmode=$MAKAR_VMODE}"

# Emit the shipped interactive grub.cfg.  Defined once and used for both the
# initial write and the post-test-ISO restore so the two can never drift.
# Layout (XP/Vista-style): GUI desktop, then "Advanced options", then "Next
# available device" at the top level; every other boot mode lives under
# "Advanced options".  Entry 0 (the
# GUI desktop) is the KERNEL_ARGS carrier and defaults to the desktop, so the
# kbtest/guitest harnesses' `GRUB_DEFAULT=0 KERNEL_ARGS=...` still select it and
# override its cmdline.  `live` marks a live-ISO session (skip the login).
_emit_interactive_grubcfg() {
	cat > isodir/boot/grub/grub.cfg << EOF
set default=${GRUB_DEFAULT:-0}
set timeout=3

# Ask GRUB for the best mode the firmware offers, highest first (the kernel
# adopts whatever LFB it's handed where there's no DISPI, e.g. Hyper-V/VMware).
insmod all_video
set gfxpayload=1280x720x32,1024x768x32,800x600x32

menuentry "Makar OS (GUI desktop)" {
	multiboot2 /boot/makar.kernel live ${KERNEL_ARGS:-autoboot=gui autologin=user}${_vmode}
}

submenu "Advanced options" {
	menuentry "Makar OS (console login)" {
		multiboot2 /boot/makar.kernel live${_vmode}
	}
	menuentry "Makar OS (verbose boot)" {
		multiboot2 /boot/makar.kernel live verbose${_vmode}
	}
	menuentry "Makar OS (sysadmin -- text console, type go32 to start the desktop)" {
		set gfxpayload=text
		multiboot2 /boot/makar.kernel live sysadmin
	}
	menuentry "Makar OS (hardware info)" {
		set gfxpayload=text
		multiboot2 /boot/makar.kernel live hwspecs
	}
	menuentry "Makar OS (rescue shell)" {
		set gfxpayload=text
		multiboot2 /boot/makar.kernel live shell=rescue
	}
	menuentry "Makar OS (serial console)" {
		multiboot2 /boot/makar.kernel live console=ttyS0${_vmode}
	}
	# Pick a specific resolution: each asks GRUB for that LFB (gfxpayload) and
	# passes vmode= so the kernel pins it via Bochs DISPI where available.
	# Unsupported modes fall back cleanly (gated on bochs_vbe_mode_supported).
	submenu "Choose resolution..." {
		menuentry "1920x1080" { set gfxpayload=1920x1080x32; multiboot2 /boot/makar.kernel live vmode=1920x1080 }
		menuentry "1600x900"  { set gfxpayload=1600x900x32;  multiboot2 /boot/makar.kernel live vmode=1600x900 }
		menuentry "1280x1024" { set gfxpayload=1280x1024x32; multiboot2 /boot/makar.kernel live vmode=1280x1024 }
		menuentry "1280x720"  { set gfxpayload=1280x720x32;  multiboot2 /boot/makar.kernel live vmode=1280x720 }
		menuentry "1024x768"  { set gfxpayload=1024x768x32;  multiboot2 /boot/makar.kernel live vmode=1024x768 }
		menuentry "800x600"   { set gfxpayload=800x600x32;   multiboot2 /boot/makar.kernel live vmode=800x600 }
	}
	menuentry "Show video modes (Hyper-V/VMware resolution diagnostic)" {
		videoinfo
		echo ""
		echo "Photograph the 'Adapter ... modes' list above -- note which widths"
		echo "(1024x768, 1280x720...) appear and at what bit depths.  That tells us"
		echo "what gfxpayload can ask for.  Returning to the menu in 60s..."
		sleep --verbose --interruptible 60
	}
}

menuentry "Next available device" {
	exit
}
EOF
}

# ── Interactive ISO ──────────────────────────────────────────────────────────
_emit_interactive_grubcfg

grub-mkrescue -o makar.iso isodir

# ── Test ISO (CI) ────────────────────────────────────────────────────────────
# Single entry, zero timeout: QEMU boots straight into ktest_run_all().
if [ "${TEST_ISO:-0}" = "1" ]; then
    # TEST_CMDLINE overrides the default test-mode cmdline.  Used by
    # `./run.sh test <name>` to boot only the named suite (e.g.
    # `test_mode test=libc-tcc`).  Default exercises every script.
    _test_cmdline="${TEST_CMDLINE:-test_mode}"
    cat > isodir/boot/grub/grub.cfg << EOF
set default=0
set timeout=0

menuentry "Makar OS (${_test_cmdline})" {
	multiboot2 /boot/makar.kernel ${_test_cmdline}
}
EOF
    grub-mkrescue -o makar-test.iso isodir

    # Restore the interactive grub.cfg in the staged isodir so anyone
    # inspecting the staging dir doesn't see the test variant.
    _emit_interactive_grubcfg
fi
