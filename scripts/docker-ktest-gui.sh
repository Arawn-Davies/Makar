#!/bin/bash
# docker-ktest-gui.sh – graphical ktest runner.
#
# Builds a TEST_MODE ISO inside Docker (same cross-compiler as CI), then runs
# QEMU on the host so the framebuffer window is visible while the test suite
# executes.  Serial output is captured to ktest.log and checked for PASS/FAIL.
#
# The kernel exits QEMU automatically via isa-debug-exit when ktest_run_all()
# finishes, so the window closes on its own.  A 60-second timeout kills QEMU
# if the kernel hangs instead.
#
# Prerequisites:
#   • Docker (for the cross-compiler build)
#   • qemu-system-i386 on the host PATH
#   • A display server (X11 / WSLg / XQuartz) for the QEMU window
#
# Usage: ./docker-ktest-gui.sh

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_BIN=${DOCKER_BIN:-docker}
DOCKER_PLATFORM=${DOCKER_PLATFORM:-linux/amd64}
export DOCKER_PLATFORM

if ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    echo "ERROR: Docker CLI not found (expected '$DOCKER_BIN')." >&2
    exit 1
fi

. "$REPO_ROOT/src/config.sh"
QEMU_BIN="qemu-system-$(bash "$REPO_ROOT/src/target-triplet-to-arch.sh" "$HOST")"

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    echo "ERROR: host QEMU not found (expected '$QEMU_BIN')." >&2
    echo "       Install it with: sudo apt install qemu-system-x86" >&2
    exit 1
fi

# ── Build TEST_MODE ISO inside Docker ─────────────────────────────────────────
echo "==> Cleaning build artifacts..."
"$DOCKER_BIN" run --rm \
    --platform "$DOCKER_PLATFORM" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '. ./src/config.sh && for p in $PROJECTS; do (cd $p && $MAKE clean 2>/dev/null || true); done'

echo "==> Building TEST_MODE ISO..."
CFLAGS='-O0 -g3' CPPFLAGS='-DTEST_MODE' DOCKER_PLATFORM="$DOCKER_PLATFORM" \
    "$REPO_ROOT/docker-iso.sh"

# ── Run QEMU on the host with a display ───────────────────────────────────────
echo "==> Running ktest suite in QEMU (graphical)..."
echo "    (window will close automatically when tests finish)"

rm -f "$REPO_ROOT/ktest.log"

# Write serial to a file rather than piping through tee - piping stdout breaks
# SDL/GTK display initialisation because QEMU detects it is not connected to a
# TTY and refuses to open a window.
#
# -serial file:...  → serial output goes directly to ktest.log
# no -display flag  → QEMU auto-selects SDL/GTK/Cocoa; override via QEMU_DISPLAY
#
# isa-debug-exit causes QEMU to exit automatically when ktest finishes.
# A background watchdog kills QEMU after 60 s in case the kernel hangs.
"$QEMU_BIN" \
    -cdrom "$REPO_ROOT/makar.iso" \
    -serial "file:$REPO_ROOT/ktest.log" \
    ${QEMU_DISPLAY:+-display "$QEMU_DISPLAY"} \
    -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    &
QEMU_PID=$!
( sleep 60 && kill "$QEMU_PID" 2>/dev/null ) &
WATCHDOG_PID=$!
wait "$QEMU_PID" 2>/dev/null || true
kill "$WATCHDOG_PID" 2>/dev/null || true
wait "$WATCHDOG_PID" 2>/dev/null || true

# ── Check result ──────────────────────────────────────────────────────────────
echo ""
if grep -q "KTEST_RESULT: PASS" "$REPO_ROOT/ktest.log"; then
    echo "==> ktest: ALL PASSED"
elif grep -q "KTEST_RESULT: FAIL" "$REPO_ROOT/ktest.log"; then
    echo "==> ktest: FAILED - see ktest.log"
    exit 1
else
    echo "==> ktest: TIMEOUT or no result - see ktest.log"
    exit 1
fi
