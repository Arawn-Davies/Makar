#!/bin/bash
# docker-test.sh – run the full test suite entirely inside Docker.
#
# Steps:
#   1. ktest suite  – TEST_MODE ISO, QEMU exits automatically via isa-debug-exit
#   2. GDB tests    – normal debug ISO, GDB automation over the QEMU stub
#
# Usage: ./docker-test.sh
#
# Requirements (host): docker

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_BIN=${DOCKER_BIN:-docker}

# ── 1. In-kernel test suite (TEST_MODE) ──────────────────────────────────────
echo "==> Step 1: ktest suite (TEST_MODE, QEMU exits when done)..."
CFLAGS='-O0 -g3' "$REPO_ROOT/docker-ktest.sh"

# ── 2. GDB boot test suite (normal build) ────────────────────────────────────
echo "==> Step 2: building debug ISO for GDB tests..."
export CFLAGS='-O0 -g3'
./docker-iso.sh

echo "==> Step 2: running GDB boot test suite..."
"$DOCKER_BIN" run --rm \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '
        qemu-system-i386 \
            -cdrom makar.iso \
            -serial file:/work/gdb-serial.log \
            -display none \
            -no-reboot \
            -no-shutdown \
            -s -S &
        QEMU_PID=$!

        sleep 2

        timeout 60 gdb-multiarch -batch \
            -ex "source tests/gdb_boot_test.py" \
            src/kernel/makar.kernel \
            2>&1 | tee /work/gdb-test.log
        GDB_EXIT=${PIPESTATUS[0]}

        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true

        exit "$GDB_EXIT"
    '
GDB_EXIT=$?

if [ "$GDB_EXIT" -eq 0 ]; then
    echo "==> All GDB tests PASSED."
else
    echo "==> GDB tests FAILED (exit $GDB_EXIT). See gdb-test.log for details."
fi

exit "$GDB_EXIT"
