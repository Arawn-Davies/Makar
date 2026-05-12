#!/bin/bash
# docker-ktest.sh – comprehensive CI test suite (everything).
#
# Step 1: in-kernel ktest suite
#   Builds a TEST_MODE ISO.  The kernel runs ktest_run_all() - all subsystem
#   unit tests including a live ring-3 execution - then exits QEMU cleanly via
#   isa-debug-exit.
#
# Step 2: GDB boot-checkpoint tests
#   Builds a normal debug ISO.  Launches QEMU with the GDB stub, then runs
#   gdb_boot_test.py to verify Multiboot 2 magic, symbol addresses, and every
#   major boot checkpoint from outside the kernel.
#
# Exit codes: 0 = everything passed, 1 = any failure or timeout.
#
# Usage: ./docker-ktest.sh

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

# ── Step 1: in-kernel ktest suite (TEST_MODE) ─────────────────────────────────
echo "==> Step 1: cleaning build artifacts..."
"$DOCKER_BIN" run --rm \
    --platform "$DOCKER_PLATFORM" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '. ./src/config.sh && for p in $PROJECTS; do (cd $p && $MAKE clean 2>/dev/null || true); done'

echo "==> Step 1: building TEST_MODE ISO..."
CFLAGS='-O0 -g3' CPPFLAGS='-DTEST_MODE' DOCKER_PLATFORM="$DOCKER_PLATFORM" ./docker-iso.sh

echo "==> Step 1: running ktest suite in QEMU (headless)..."
"$DOCKER_BIN" run --rm \
    --platform "$DOCKER_PLATFORM" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '
        timeout 30 qemu-system-i386 \
            -cdrom makar.iso \
            -serial stdio \
            -display none \
            -no-reboot \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            2>/dev/null | tee /work/ktest.log || true

        if grep -q "KTEST_RESULT: PASS" /work/ktest.log; then
            echo "==> ktest: ALL PASSED"
        elif grep -q "KTEST_RESULT: FAIL" /work/ktest.log; then
            echo "==> ktest: FAILED - see ktest.log"
            exit 1
        else
            echo "==> ktest: TIMEOUT or no result - see ktest.log"
            exit 1
        fi
    '

# ── Step 2: GDB boot-checkpoint tests (normal debug build) ────────────────────
echo "==> Step 2: cleaning TEST_MODE artifacts..."
"$DOCKER_BIN" run --rm \
    --platform "$DOCKER_PLATFORM" \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '. ./src/config.sh && for p in $PROJECTS; do (cd $p && $MAKE clean 2>/dev/null || true); done'

echo "==> Step 2: building debug ISO for GDB tests..."
CFLAGS='-O0 -g3' DOCKER_PLATFORM="$DOCKER_PLATFORM" ./docker-iso.sh

echo "==> Step 2: running GDB boot-checkpoint tests..."
"$DOCKER_BIN" run --rm \
    --platform "$DOCKER_PLATFORM" \
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

echo "==> All tests PASSED."
