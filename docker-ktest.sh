#!/bin/bash
# docker-ktest.sh – build a TEST_MODE ISO and run the in-kernel test suite.
#
# The kernel boots, runs ktest_run_all(), writes KTEST_RESULT: PASS/FAIL to
# serial, then triggers a clean QEMU exit via the isa-debug-exit device.
#
# Exit codes: 0 = all tests passed, 1 = one or more tests failed / timeout.
#
# Usage: ./docker-ktest.sh

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DOCKER_IMAGE=${DOCKER_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_BIN=${DOCKER_BIN:-docker}

if ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
    echo "ERROR: Docker CLI not found (expected '$DOCKER_BIN')." >&2
    exit 1
fi

echo "==> Building TEST_MODE ISO..."
CFLAGS='-O0 -g3' CPPFLAGS='-DTEST_MODE' ./docker-iso.sh

echo "==> Running ktest suite in QEMU (headless)..."
"$DOCKER_BIN" run --rm \
    -v "$REPO_ROOT:/work" \
    -w /work \
    "$DOCKER_IMAGE" \
    bash -c '
        # isa-debug-exit: kernel writes 0 (pass) or 1 (fail) to port 0xF4.
        # QEMU exit code = (written_val << 1) | 1 → 1 (pass) or 3 (fail).
        timeout 30 qemu-system-i386 \
            -cdrom makar.iso \
            -serial stdio \
            -display none \
            -no-reboot \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            2>/dev/null | tee /work/ktest.log || true

        if grep -q "KTEST_RESULT: PASS" /work/ktest.log; then
            echo "==> ktest: ALL PASSED"
            exit 0
        elif grep -q "KTEST_RESULT: FAIL" /work/ktest.log; then
            echo "==> ktest: FAILED — see ktest.log"
            exit 1
        else
            echo "==> ktest: TIMEOUT or no result — see ktest.log"
            exit 1
        fi
    '
