#!/bin/sh
# build-musl-demos.sh -- compile the musl demo programs with the hosted cross
# toolchain and stage them into isodir/apps/ so the ISO ships them.
#
# Separate from the kernel build: these link against *real musl* via
# toolchain/cc.sh (i686-linux-musl-gcc), not the freestanding i686-elf-gcc +
# libc.a shim the in-tree apps use.  Run this *before* `./run.sh iso build`
# (iso.sh only mkdir's isodir/apps, so staged files survive packaging).
#
# Static demos must be linked at >= 0x10000000 (Makar reserves 0..256 MiB as the
# low identity window); we use 0x40000000, the same base the in-tree apps use.
# Dynamic demos are PIE -- the kernel picks their base, so no -Ttext needed.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$DIR"
mkdir -p isodir/apps

CC="toolchain/cc.sh"
CFLAGS="-march=i686 -O2 -Wall"

# NB: keep the names hyphen-free, like the in-tree apps -- avoids any ISO9660
# name-resolution surprises.  Static demo (Phase 0): muslhello.elf.
$CC $CFLAGS -static -Wl,-Ttext-segment=0x40000000 \
    toolchain/test/hello.c -o isodir/apps/muslhello.elf
echo "==> staged isodir/apps/muslhello.elf     (static musl, base 0x40000000)"

# --- Phase 1: dynamic musl hello (PIE; PT_INTERP=/lib/ld-musl-i386.so.1) ----
# Built but only runnable once the kernel's dynamic loader + /lib/libc.so land.
# -pie -fPIE forces ET_DYN so the kernel relocates it above the low 256 MiB
# identity window (a non-PIE dynamic ET_EXEC would link at 0x08048000).
$CC $CFLAGS -pie -fPIE toolchain/test/hello.c -o isodir/apps/muslhellodyn.elf
echo "==> staged isodir/apps/muslhellodyn.elf  (dynamic musl PIE, PT_INTERP)"
