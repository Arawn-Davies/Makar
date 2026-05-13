#!/bin/bash
# run.sh - single entrypoint for building, testing, and running Makar OS.
#
# Usage: ./run.sh <mode>
#
# Modes:
#   iso-boot       Clean, build interactive ISO, run in QEMU
#   iso-test       Full ISO CI suite: ktest (TEST_MODE) + GDB boot-checkpoint
#   iso-ktest-gui  Build TEST_MODE ISO + run ktest with a display window
#   iso-release    Build optimised release ISO
#   hdd-boot       Clean, build HDD image, run QEMU from disk
#   hdd-test       Build fresh HDD test image + GDB boot test
#   hdd-release    Build HDD image only
#   clean          Remove all build artefacts
#
# Execution strategy (checked in order):
#
#   Build steps
#     1. /.dockerenv present (container / CI)  → run directly
#     2. Docker CLI available                  → wrap in 'docker run'
#     3. i686-elf-gcc present (native tools)   → run directly
#     4. Otherwise                             → error with install hints
#
#   QEMU steps
#     1. qemu-system-i386 on host PATH         → run on host
#     2. Docker available                      → run inside container
#     3. Already in container / native tools   → run directly
#
#   GDB test steps (need gdb-multiarch too)
#     1. host qemu-system-i386 + gdb-multiarch → run on host
#     2. Docker / container / native tools     → run inside container
#
# Environment overrides (all optional):
#   DOCKER_IMAGE      build container  (default: arawn780/gcc-cross-i686-elf:fast)
#   DOCKER_BIN        docker CLI       (default: docker)
#   DOCKER_PLATFORM   platform flag    (default: linux/amd64)
#   HDD_IMG           interactive HDD  (default: makar-hdd.img)
#   HDD_TEST_IMG      CI test HDD      (default: makar-hdd-test.img)
#   QEMU_DISPLAY      passed to -display for iso-ktest-gui

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# makar-build:local layers ccache on top of the upstream toolchain image so
# rebuilds hit the object cache instead of recompiling from scratch.  It is
# auto-built on first use; set DOCKER_IMAGE explicitly to override.
DOCKER_IMAGE=${DOCKER_IMAGE:-makar-build:local}
DOCKER_UPSTREAM_IMAGE=${DOCKER_UPSTREAM_IMAGE:-arawn780/gcc-cross-i686-elf:fast}
DOCKER_BIN=${DOCKER_BIN:-docker}
DOCKER_PLATFORM=${DOCKER_PLATFORM:-linux/amd64}
HDD_IMG=${HDD_IMG:-makar-hdd.img}
HDD_TEST_IMG=${HDD_TEST_IMG:-makar-hdd-test.img}
export DOCKER_PLATFORM

MODE="${1:-}"
if [ -z "$MODE" ]; then
    echo "Usage: $0 <mode>"
    echo "Modes: iso-boot  iso-test  iso-ktest-gui  iso-release"
    echo "       hdd-boot  hdd-test  hdd-release    clean"
    exit 1
fi

# ── context helpers ────────────────────────────────────────────────────────────

# Build execution context: container | docker | native | none
_build_ctx() {
    if [ -f /.dockerenv ]; then
        printf 'container'
    elif command -v "$DOCKER_BIN" >/dev/null 2>&1; then
        printf 'docker'
    elif command -v i686-elf-gcc >/dev/null 2>&1; then
        printf 'native'
    else
        printf 'none'
    fi
}

# Return the host QEMU binary name, or empty string if not found.
_host_qemu() {
    # shellcheck source=src/config.sh
    . "$REPO_ROOT/src/config.sh"
    local _b="qemu-system-$(bash "$REPO_ROOT/src/target-triplet-to-arch.sh" "$HOST")"
    command -v "$_b" >/dev/null 2>&1 && printf '%s' "$_b" || true
}

# Return gdb-multiarch path, or empty string.
_host_gdb() {
    command -v gdb-multiarch >/dev/null 2>&1 && printf 'gdb-multiarch' || true
}

# ── _drun: run a build command in the right context ───────────────────────────
#
# Usage: _drun [--privileged] [--as-root] [--env K=V]... -- "cmd string"
#
#   --privileged   pass --privileged to docker run  (loop-device / HDD work)
#   --as-root      omit -u UID:GID so the container process runs as root
#   --env K=V      forward an environment variable into the container
#
# In container / native context the flags are ignored; the command runs via
# 'bash -lc' in the current working directory.

_ensure_build_image() {
    [ "$(_build_ctx)" = "docker" ] || return 0
    if "$DOCKER_BIN" image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
        return 0
    fi
    echo "==> Building $DOCKER_IMAGE (one-time, layers ccache on $DOCKER_UPSTREAM_IMAGE)..."
    "$DOCKER_BIN" build \
        --platform "$DOCKER_PLATFORM" \
        -t "$DOCKER_IMAGE" \
        "$REPO_ROOT"
}

_drun() {
    local _priv="" _user="-u $(id -u):$(id -g)"
    local -a _env=()

    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        case "$1" in
            --privileged) _priv="--privileged" ;;
            --as-root)    _user="" ;;
            --env)        _env+=("-e" "$2"); shift ;;
        esac
        shift
    done
    [ "${1:-}" = "--" ] && shift
    local _cmd="${1:?_drun: missing command}"

    case "$(_build_ctx)" in
        container|native)
            bash -lc "$_cmd"
            ;;
        docker)
            _ensure_build_image
            # shellcheck disable=SC2086
            "$DOCKER_BIN" run --rm \
                --platform "$DOCKER_PLATFORM" \
                ${_priv} \
                ${_user} \
                "${_env[@]}" \
                -v "$REPO_ROOT:/work" -w /work \
                "$DOCKER_IMAGE" \
                bash -lc "$_cmd"
            ;;
        none)
            echo "ERROR: Docker not found and cross-compiler (i686-elf-gcc) not installed." >&2
            echo "       Install Docker, or install the native build prerequisites:" >&2
            echo "         i686-elf-gcc  grub-mkrescue  xorriso" >&2
            echo "         qemu-system-i386  gdb-multiarch" >&2
            exit 1
            ;;
    esac
}

# ── QEMU / GDB run helpers ────────────────────────────────────────────────────

# Run an interactive QEMU boot (serial stdio).
# Pass QEMU args using /work/ as the path prefix for image files.
# Prefers host QEMU; falls back to Docker (-it) or direct execution.
_run_qemu_interactive() {
    local _args="$1"
    local _qemu
    _qemu=$(_host_qemu)

    if [ -n "$_qemu" ]; then
        local _host_args="${_args//\/work\//$REPO_ROOT/}"
        # shellcheck disable=SC2086
        "$_qemu" $_host_args
    elif [ "$(_build_ctx)" = "docker" ]; then
        echo "==> Host QEMU not found - running QEMU in Docker (serial stdio)..."
        "$DOCKER_BIN" run --rm -it \
            --platform "$DOCKER_PLATFORM" \
            -v "$REPO_ROOT:/work" -w /work \
            "$DOCKER_IMAGE" \
            bash -lc "qemu-system-i386 $_args"
    else
        bash -lc "qemu-system-i386 $_args"
    fi
}

# Run the ktest suite (headless QEMU + serial capture).
# QEMU exits via isa-debug-exit when the kernel finishes, or via -no-reboot
# on panic/triple-fault.  No timeout needed.
_run_ktest() {
    echo "==> Running ktest suite (headless QEMU)..."
    local _qemu
    _qemu=$(_host_qemu)
    rm -f "$REPO_ROOT/ktest.log"

    if [ -n "$_qemu" ]; then
        "$_qemu" \
            -cdrom "$REPO_ROOT/makar.iso" \
            -serial "file:$REPO_ROOT/ktest.log" \
            -display none \
            -no-reboot \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            2>/dev/null || true
    else
        _drun --as-root -- \
            'qemu-system-i386 \
                 -cdrom makar.iso \
                 -serial file:/work/ktest.log \
                 -display none \
                 -no-reboot \
                 -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
                 2>/dev/null || true'
    fi
    _check_ktest
}

_check_ktest() {
    if grep -q "KTEST_RESULT: PASS" "$REPO_ROOT/ktest.log"; then
        echo "==> ktest: ALL PASSED"
    elif grep -q "KTEST_RESULT: FAIL" "$REPO_ROOT/ktest.log"; then
        echo "==> ktest: FAILED - see ktest.log"; exit 1
    else
        echo "==> ktest: TIMEOUT or no result - see ktest.log"; exit 1
    fi
}

# Create a minimal 32 MiB FAT32 disk image for the ISO GDB test.
# Uses mkfs.fat --offset (sector-based, no losetup / no --privileged needed)
# so this works inside the GitHub Actions container job.
_make_fat32_disk() {
    local _img="${1:-iso-test-hdd.img}"
    echo "==> Creating 32 MiB FAT32 test disk ($REPO_ROOT/$_img)..."
    rm -f "$REPO_ROOT/$_img"
    _drun -- \
        "IMG='/work/$_img'
         truncate -s 32M \"\$IMG\"
         printf 'label: dos\nstart=2048, type=c\n' | sfdisk \"\$IMG\" >/dev/null 2>&1
         mkfs.fat -F 32 -n MAKAR --offset 2048 \"\$IMG\" >/dev/null
         echo '  FAT32 test disk ready.'"
}

# Run the GDB ISO boot-checkpoint test.
# Boots from CD-ROM only - verifies boot sequence, background ktest, and
# CD-ROM filesystem content.
_run_gdb_iso_test() {
    echo "==> Running GDB boot tests (ISO boot)..."
    local _qemu _gdb
    _qemu=$(_host_qemu)
    _gdb=$(_host_gdb)

    if [ -n "$_qemu" ] && [ -n "$_gdb" ]; then
        "$_qemu" \
            -drive "file=$REPO_ROOT/makar.iso,if=ide,index=2,media=cdrom" \
            -boot order=d \
            -serial "file:$REPO_ROOT/gdb-serial.log" \
            -display none -no-reboot -no-shutdown \
            -s -S &
        QPID=$!
        sleep 2
        timeout 120 "$_gdb" -batch \
            -ex "source $REPO_ROOT/tests/gdb_boot_test.py" \
            "$REPO_ROOT/src/kernel/makar.kernel" \
            2>&1 | tee "$REPO_ROOT/gdb-test.log"
        RC=${PIPESTATUS[0]}
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        return $RC
    else
        _drun --as-root -- \
            'qemu-system-i386 \
                 -drive file=/work/makar.iso,if=ide,index=2,media=cdrom \
                 -boot order=d \
                 -serial file:/work/gdb-serial.log \
                 -display none -no-reboot -no-shutdown \
                 -s -S &
             QPID=$!
             sleep 2
             timeout 120 gdb-multiarch -batch \
                 -ex "source tests/gdb_boot_test.py" \
                 src/kernel/makar.kernel \
                 2>&1 | tee /work/gdb-test.log
             RC=${PIPESTATUS[0]}
             kill "$QPID" 2>/dev/null || true
             wait "$QPID" 2>/dev/null || true
             exit "$RC"'
    fi
}

# Run the GDB HDD boot test.  Same host-first / Docker-fallback logic.
_run_gdb_hdd_test() {
    local _img="$1"
    echo "==> Booting $_img under GDB..."
    local _qemu _gdb
    _qemu=$(_host_qemu)
    _gdb=$(_host_gdb)

    if [ -n "$_qemu" ] && [ -n "$_gdb" ]; then
        "$_qemu" \
            -drive "file=$REPO_ROOT/$_img,format=raw,if=ide,index=0" \
            -boot c \
            -serial "file:$REPO_ROOT/hdd-test-serial.log" \
            -display none -no-reboot -no-shutdown \
            -s -S &
        QPID=$!
        sleep 2
        timeout 120 "$_gdb" -batch \
            -ex "source $REPO_ROOT/tests/gdb_hdd_test.py" \
            "$REPO_ROOT/src/kernel/makar.kernel" \
            2>&1 | tee "$REPO_ROOT/hdd-test-gdb.log"
        RC=${PIPESTATUS[0]}
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        return $RC
    else
        _drun --as-root -- \
            "qemu-system-i386 \
                 -drive file=/work/$_img,format=raw,if=ide,index=0 \
                 -boot c \
                 -serial file:/work/hdd-test-serial.log \
                 -display none -no-reboot -no-shutdown \
                 -s -S &
             QPID=\$!
             sleep 2
             timeout 120 gdb-multiarch -batch \
                 -ex 'source tests/gdb_hdd_test.py' \
                 src/kernel/makar.kernel \
                 2>&1 | tee /work/hdd-test-gdb.log
             RC=\${PIPESTATUS[0]}
             kill \"\$QPID\" 2>/dev/null || true
             wait \"\$QPID\" 2>/dev/null || true
             exit \"\$RC\""
    fi
}

# ── shared build steps ────────────────────────────────────────────────────────

_clean() {
    echo "==> Cleaning build artefacts..."
    _drun --as-root -- \
        '. ./src/config.sh
         for p in $PROJECTS; do (cd "$p" && $MAKE clean 2>/dev/null || true); done'
}

_build_iso() {
    local _flags="${1:-}"
    echo "==> Building ISO${_flags:+ ($_flags)}..."
    _drun -- "${_flags:+$_flags }bash iso.sh"
}

_build_kernel() {
    local _flags="${1:-}"
    echo "==> Building kernel${_flags:+ ($_flags)}..."
    _drun -- "${_flags:+$_flags }bash build.sh"
}

# ── modes ──────────────────────────────────────────────────────────────────────

case "$MODE" in

# ── iso-boot ──────────────────────────────────────────────────────────────────
iso-boot)
    _clean
    _build_iso "CFLAGS='-O0 -g3'"
    if [ ! -f "$REPO_ROOT/hdd.img" ]; then
        _drun --as-root -- "qemu-img create -f raw hdd.img 512M"
    fi
    _run_qemu_interactive \
        "-drive file=/work/hdd.img,format=raw,if=ide,index=0 \
         -drive file=/work/makar.iso,if=ide,index=2,media=cdrom \
         -boot order=d -serial stdio"
    ;;

# ── iso-test ──────────────────────────────────────────────────────────────────
iso-test)
    _clean
    _build_iso "CFLAGS='-O0 -g3' KERNEL_ARGS=test_mode"
    _run_ktest
    _build_iso "CFLAGS='-O0 -g3'"
    _run_gdb_iso_test
    echo "==> All ISO tests PASSED."
    ;;

# ── iso-ktest-gui ─────────────────────────────────────────────────────────────
# Requires host QEMU and a display server - cannot fall back to Docker.
iso-ktest-gui)
    QEMU_BIN=$(_host_qemu)
    if [ -z "$QEMU_BIN" ]; then
        echo "ERROR: iso-ktest-gui requires host QEMU and a display server." >&2
        echo "       Install qemu-system-i386 with X11/SDL/Cocoa support." >&2
        exit 1
    fi
    _clean
    _build_iso "CFLAGS='-O0 -g3' KERNEL_ARGS=test_mode"
    echo "==> Running ktest suite (graphical QEMU - window closes on completion)..."
    rm -f "$REPO_ROOT/ktest.log"
    "$QEMU_BIN" \
        -cdrom "$REPO_ROOT/makar.iso" \
        -serial "file:$REPO_ROOT/ktest.log" \
        ${QEMU_DISPLAY:+-display "$QEMU_DISPLAY"} \
        -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 &
    QPID=$!
    ( sleep 60 && kill "$QPID" 2>/dev/null ) &
    WPID=$!
    wait "$QPID" 2>/dev/null || true
    kill "$WPID" 2>/dev/null || true
    wait "$WPID" 2>/dev/null || true
    _check_ktest
    ;;

# ── iso-release ───────────────────────────────────────────────────────────────
iso-release)
    _clean
    _build_iso "CFLAGS='-O2 -g'"
    echo "==> Release ISO ready: $REPO_ROOT/makar.iso"
    ;;

# ── hdd-boot ──────────────────────────────────────────────────────────────────
hdd-boot)
    _clean
    _build_kernel "CFLAGS='-O0 -g3'"
    rm -f "$REPO_ROOT/$HDD_IMG"
    HDD_IMG="$HDD_IMG" DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    _run_qemu_interactive \
        "-drive file=/work/$HDD_IMG,format=raw,if=ide,index=0 \
         -boot c -serial stdio"
    ;;

# ── hdd-test ──────────────────────────────────────────────────────────────────
hdd-test)
    _clean
    _build_kernel "CFLAGS='-O0 -g3'"
    if [ ! -f "$REPO_ROOT/src/kernel/makar.kernel" ]; then
        echo "ERROR: src/kernel/makar.kernel not found after build." >&2; exit 1
    fi
    rm -f "$REPO_ROOT/$HDD_TEST_IMG"
    HDD_IMG="$HDD_TEST_IMG" DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    _run_gdb_hdd_test "$HDD_TEST_IMG"
    echo ""
    echo "==> HDD boot test PASSED."
    echo "    GDB log:    hdd-test-gdb.log"
    echo "    Serial log: hdd-test-serial.log"
    ;;

# ── hdd-release ───────────────────────────────────────────────────────────────
hdd-release)
    _clean
    _build_kernel "CFLAGS='-O2 -g'"
    rm -f "$REPO_ROOT/$HDD_IMG"
    HDD_IMG="$HDD_IMG" DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    echo "==> HDD image ready: $REPO_ROOT/$HDD_IMG"
    ;;

# ── clean ─────────────────────────────────────────────────────────────────────
clean)
    _clean
    rm -rf "$REPO_ROOT/sysroot" "$REPO_ROOT/isodir" \
           "$REPO_ROOT/makar.iso" "$REPO_ROOT/hdd.img"
    echo "==> Clean complete."
    ;;

*)
    echo "ERROR: unknown mode '$MODE'" >&2
    echo "Modes: iso-boot  iso-test  iso-ktest-gui  iso-release" >&2
    echo "       hdd-boot  hdd-test  hdd-release    clean" >&2
    exit 1
    ;;
esac
