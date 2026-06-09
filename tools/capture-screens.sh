#!/usr/bin/env bash
# Capture boot-mode screenshots into images/ via headless QEMU `screendump`s.
#
# Reproducible: boots makar.iso headless, drives the QEMU monitor over a FIFO
# (no host keyboard), screendumps to PPM at a timed point, converts to PNG.
# Text modes rebuild the ISO so entry 0's cmdline carries the boot flag.
# Needs host qemu-system-i386 + python3.  Run from the repo root: tools/capture-screens.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"; cd "$REPO"
mkdir -p images
QEMU="${QEMU:-qemu-system-i386}"
command -v "$QEMU" >/dev/null 2>&1 || { echo "need host $QEMU"; exit 1; }

ACCEL="-accel tcg"
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then ACCEL="-enable-kvm -cpu host"; fi
echo "accel: $ACCEL"

# shoot <wait_secs> <out.png> -- boot the current makar.iso, screendump after N s.
shoot() {
  local wait="$1" out="$2"
  local fifo ppm; fifo="$(mktemp -u)"; ppm="$(mktemp -u).ppm"; mkfifo "$fifo"
  ( sleep "$wait"; echo "screendump $ppm"; sleep 2; echo "quit"; sleep 1 ) > "$fifo" &
  local wpid=$!
  "$QEMU" -cdrom "$REPO/makar.iso" -m 256 -vga std $ACCEL -display none \
          -monitor stdio -no-reboot < "$fifo" > /dev/null 2>&1 || true
  wait "$wpid" 2>/dev/null || true
  if [ -f "$ppm" ] && python3 "$REPO/tools/ppm2png.py" "$ppm" "$REPO/images/$out" >/dev/null 2>&1; then
    echo "  captured images/$out"
  else
    echo "  !! capture FAILED: $out"
  fi
  rm -f "$fifo" "$ppm"
}

build_mode() {  # <KERNEL_ARGS>
  echo "== rebuild ISO (KERNEL_ARGS=$1) =="
  GRUB_DEFAULT=0 KERNEL_ARGS="$1" ./run.sh iso build > /dev/null 2>&1
}

# 1) GUI from the current (GUI-default) makar.iso -- no rebuild.
echo "== GUI desktop + splash (current ISO) =="
shoot 12 gui-splash.png      # mid-boot: graphical emblem splash
shoot 30 gui-desktop.png     # settled: the makx desktop + dock tray

# 2) Text modes -- rebuild entry 0's cmdline, then capture the settled screen.
build_mode "verbose";              shoot 24 boot-verbose.png
build_mode "sysadmin";             shoot 24 mode-sysadmin.png
build_mode "hwspecs";              shoot 16 mode-hwspecs.png

# 3) Restore the shipped GUI-default ISO.
build_mode "autoboot=gui autologin=user"
echo "== done; images/ =="
ls -la "$REPO/images/"
