#!/bin/bash
# run.sh - single entrypoint for building, testing, and running Makar OS.
#
# Usage: ./run.sh <mode>
#
# Modes:
#   iso-boot       Clean, build interactive ISO, run in QEMU
#   iso-test       Full ISO CI suite: ktest (test_mode boot) + GDB boot-checkpoint
#   iso-ktest-gui  Build test_mode ISO + run ktest with a display window
#   iso-release    Build optimised release ISO
#   iso-build      Build kernel + makar.iso + makar-test.iso (no run)
#   hdd-boot       Clean, build HDD image, run QEMU from disk
#   hdd-test       Build fresh HDD test image + GDB boot test
#   hdd-release    Build HDD image only
#   hdd-build      Build kernel + makar-hdd-test.img (no run)
#   ktest-run      Run ktest against an existing makar-test.iso
#   gdb-iso-run    Run GDB ISO boot test against existing makar.iso
#   gdb-hdd-run    Run GDB HDD boot test against existing makar-hdd-test.img
#   ui-test        Black-box UI tests (headless QEMU + HMP sendkey + serial grep)
#   ui-test-gui    Same as ui-test with visible QEMU window + paced typing
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
#   QEMU_DISPLAY      passed to -display for iso-ktest-gui and ui-test-gui
#                     (e.g. cocoa on macOS, gtk on X11/Wayland)
#   KEY_DELAY         inter-keystroke pause for ui-test-gui (default 0.15 s)

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
# Single source of truth for HDD image sizes.  Release images carry the full
# bootfs (32 MiB: limine + multiple kernels + breathing room) and rootfs
# (224 MiB: apps + src + docs + user data + headroom).  CI / iso-test
# scratch images are sized for fast artifact upload but still big enough to
# accept the full apps tree (bootfs 16 + rootfs 48 = 64 MiB).
MAKAR_HDD_SIZE_MB=${MAKAR_HDD_SIZE_MB:-96}
MAKAR_HDD_TEST_SIZE_MB=${MAKAR_HDD_TEST_SIZE_MB:-96}
export DOCKER_PLATFORM MAKAR_HDD_SIZE_MB MAKAR_HDD_TEST_SIZE_MB

# Portable bounded-run wrapper for host commands.  GNU coreutils ships
# `timeout(1)`; macOS doesn't, but Homebrew coreutils provides `gtimeout`.
# Falls back to running the command unbounded if neither is present (better
# than the whole step erroring out with "timeout: command not found").
# Usage: _timeout <seconds> <cmd> [args...]
_timeout() {
    local _secs=$1; shift
    if   command -v timeout  >/dev/null 2>&1; then timeout  "$_secs" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$_secs" "$@"
    else "$@"; fi
}

# Argument grammar: `./run.sh <target> <verb> [args...]`.  Linux-build-
# style: incremental by default (make handles "did anything change");
# only `clean` wipes artefacts.
#
#   iso build                    iso boot                iso test
#   iso release
#   hdd build                    hdd boot                hdd test
#   hdd release
#   gdb iso                      gdb hdd
#   ktest                        ktest graphical
#   ui [scenarios...]            ui graphical [scenarios...]
#   clean
_usage() {
    echo "Usage: $0 <target> <verb> [args...]"
    echo ""
    echo "  iso   build | boot | test | release"
    echo "  hdd   build | boot | test | release"
    echo "  gdb   iso | hdd"
    echo "  ktest [graphical]"
    echo "  ui    [scenario_or_group...]    -- headless ui tests"
    echo "  gui   [scenario_or_group...]    -- ui tests with visible QEMU window"
    echo "        groups: all (default) | fast | shell | cd_pwd | fs | posix | libc | vt | bughunt"
    echo "  all   [graphical]   -- gdb checkpoints + bg-ktest + ui in one QEMU"
    echo "  clean"
    echo ""
    echo "Builds are incremental (make-driven).  Run \`clean\` to force"
    echo "a from-scratch rebuild."
    exit "${1:-1}"
}

case "${1:-}" in
    iso|hdd|gdb)
        if [ -z "${2:-}" ]; then _usage; fi
        MODE="$1 $2"; shift 2 ;;
    ktest)
        if [ "${2:-}" = "graphical" ]; then
            MODE="ktest graphical"; shift 2
        else
            MODE="ktest"; shift 1
        fi ;;
    ui)
        MODE="ui"; shift 1 ;;
    gui)
        # Alias for the visible-window ui-test run.  Replaces the
        # awkward `ui graphical <scenarios>` form where arg order
        # mattered (`ui foo graphical` was silently parsed as headless,
        # with `graphical` treated as a scenario name).  With `gui`,
        # any remaining args are scenarios or group names.
        MODE="ui graphical"; shift 1 ;;
    all)
        if [ "${2:-}" = "graphical" ]; then
            MODE="all graphical"; shift 2
        else
            MODE="all"; shift 1
        fi ;;
    clean)
        MODE="clean"; shift 1 ;;
    -h|--help|help|"")
        _usage 0 ;;
    *)
        echo "ERROR: unknown target '$1'" >&2
        _usage ;;
esac

# ── context helpers ────────────────────────────────────────────────────────────

# Build execution context: container | docker | native | none
#
# A "container" context only counts when the cross-compiler is actually
# present.  This matters for tools like `act` that wrap every job in a
# generic ubuntu image (which has /.dockerenv but no i686-elf-gcc): in
# that case we fall through to the docker path and shell into the
# toolchain image just as we would on a host runner.
_build_ctx() {
    if [ -f /.dockerenv ] && command -v i686-elf-gcc >/dev/null 2>&1; then
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
    if command -v gdb-multiarch >/dev/null 2>&1; then
        printf 'gdb-multiarch'
    elif command -v gdb >/dev/null 2>&1; then
        # macOS via brew: plain `gdb` supports remote :1234 against the
        # QEMU stub for i386 -- no ptrace involved, so the codesigning
        # caveat doesn't apply.
        printf 'gdb'
    fi
}

# Emit "-accel kvm" when /dev/kvm is usable AND MAKAR_USE_KVM=1.  Disabled
# by default because (a) the QEMU GDB stub's software breakpoints fail to
# insert reliably under KVM (early-boot _start breakpoint never catches),
# and (b) a yet-to-be-fixed kernel race surfaces under KVM's true-CPU
# timing, manifesting as a page fault during ktest with a corrupted SS
# selector.  Both issues are tracked separately; until they're addressed,
# stick with TCG so CI is deterministic.
_qemu_accel() {
    [ "${MAKAR_USE_KVM:-0}" = "1" ] || return 0
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        printf -- '-accel kvm'
    fi
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
        "$_qemu" -m 32 $_host_args
    elif [ "$(_build_ctx)" = "docker" ]; then
        echo "==> Host QEMU not found - running QEMU in Docker (serial stdio)..."
        "$DOCKER_BIN" run --rm -it \
            --platform "$DOCKER_PLATFORM" \
            -v "$REPO_ROOT:/work" -w /work \
            "$DOCKER_IMAGE" \
            bash -lc "qemu-system-i386 -m 32 $_args"
    else
        bash -lc "qemu-system-i386 -m 32 $_args"
    fi
}

# Run the ktest suite (headless QEMU + serial capture).
# Uses makar-test.iso (test_mode menuentry, zero GRUB timeout) so QEMU
# boots straight into ktest_run_all().  Exits via isa-debug-exit when the
# kernel finishes, or via -no-reboot on panic/triple-fault.
_run_ktest() {
    echo "==> Running ktest suite (headless QEMU)..."
    local _qemu _iso
    _qemu=$(_host_qemu)
    _iso="${KTEST_ISO:-makar-test.iso}"
    rm -f "$REPO_ROOT/ktest.log"

    local _accel _tmo
    _accel=$(_qemu_accel)
    # ktest exits QEMU via isa-debug-exit; if the kernel hangs we still want
    # a bounded test run rather than waiting on the GitHub-Actions job
    # timeout (6h default), so wrap QEMU in `timeout` (GNU coreutils) or
    # `gtimeout` (macOS via Homebrew coreutils); only prepend if present.
    _tmo=""
    if   command -v timeout  >/dev/null 2>&1; then _tmo="timeout 120"
    elif command -v gtimeout >/dev/null 2>&1; then _tmo="gtimeout 120"
    fi
    if [ -n "$_qemu" ]; then
        # shellcheck disable=SC2086
        $_tmo "$_qemu" \
            -cdrom "$REPO_ROOT/$_iso" \
            -serial "file:$REPO_ROOT/ktest.log" \
            -display none \
            -no-reboot \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
            $_accel \
            2>/dev/null || true
    else
        _drun --as-root --env "QEMU_ACCEL=$_accel" --env "KTEST_ISO_NAME=$_iso" -- \
            'timeout 120 qemu-system-i386 \
                 -cdrom /work/$KTEST_ISO_NAME \
                 -m 32 \
                 -serial file:/work/ktest.log \
                 -display none \
                 -no-reboot \
                 -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
                 $QEMU_ACCEL \
                 2>/dev/null || true'
    fi
    _check_ktest
}

_check_ktest() {
    # Echo the per-test transcript so CI logs surface every group / PASS /
    # FAIL line instead of just the overall verdict.  The kernel emits
    # serial lines like "--- Running group: X ---", "PASS: <assert>",
    # "GROUP PASS: X", "ktest summary: N/N passed".
    # ktest format (see arch/i386/proc/ktest.c):
    #   "[ktest] suite: <name>"
    #   "  PASS: <assertion>" / "  FAIL: <assertion>"
    #   "[ktest] results: N passed, M failed"
    #   "KTEST_RESULT: PASS" / "KTEST_RESULT: FAIL"
    # incore format (see src/userspace/incore.sh, run inline after ktest_run_all):
    #   "INCORE: RUN <test>"
    #   "INCORE: PASS <test>" / "INCORE: FAIL <test>"
    #   "INCORE: ALL PASS"    / "INCORE: FAIL"
    # Plus any kernel panic / KPANIC line if a test corrupted state.
    echo "---- ktest transcript ----"
    grep -E "^(\[ktest\]|  PASS:|  FAIL:|KTEST_RESULT|INCORE:|KPANIC|kpanic)" \
        "$REPO_ROOT/ktest.log" || true
    echo "---- end ktest transcript ----"

    local ktest_status=unknown
    if grep -q "KTEST_RESULT: PASS" "$REPO_ROOT/ktest.log"; then
        ktest_status=pass
    elif grep -q "KTEST_RESULT: FAIL" "$REPO_ROOT/ktest.log"; then
        ktest_status=fail
    fi

    local incore_status=unknown
    if grep -q "^INCORE: ALL PASS" "$REPO_ROOT/ktest.log"; then
        incore_status=pass
    elif grep -q "^INCORE: FAIL" "$REPO_ROOT/ktest.log"; then
        incore_status=fail
    fi

    # LIBC-TCC: in-OS sh-script tests for the libc / TCC self-rebuild
    # matrix.  Runs inline during test_mode bootup (no HMP sendkey),
    # marker emitted by /src/userspace/libc-tcc.sh.
    local libc_status=unknown
    if grep -q "^LIBC-TCC: ALL PASS" "$REPO_ROOT/ktest.log"; then
        libc_status=pass
    elif grep -q "^LIBC-TCC: FAIL" "$REPO_ROOT/ktest.log"; then
        libc_status=fail
    fi

    case "$ktest_status:$incore_status:$libc_status" in
        pass:pass:pass)
            echo "==> ktest: ALL PASSED (+ incore + libc-tcc)" ;;
        pass:pass:unknown)
            echo "==> ktest: ALL PASSED (+ incore; libc-tcc: not run)" ;;
        pass:unknown:unknown)
            echo "==> ktest: ALL PASSED (incore: not run -- pre-incore kernel?)" ;;
        fail:*:*|*:fail:*|*:*:fail)
            echo "==> FAILED ktest=$ktest_status incore=$incore_status libc=$libc_status -- see ktest.log"; exit 1 ;;
        unknown:*:*)
            echo "==> ktest: TIMEOUT or no result - see ktest.log"; exit 1 ;;
    esac
}

# Create a minimal 32 MiB FAT32 disk image for the ISO GDB test.
# Uses mkfs.fat --offset (sector-based, no losetup / no --privileged needed)
# so this works inside the GitHub Actions container job.
_make_fat32_disk() {
    local _img="${1:-iso-test-hdd.img}"
    local _sz="${MAKAR_HDD_TEST_SIZE_MB:-64}"
    echo "==> Creating ${_sz} MiB FAT32 test disk ($REPO_ROOT/$_img)..."
    rm -f "$REPO_ROOT/$_img"
    _drun --env "MAKAR_DISK_MB=$_sz" -- \
        "IMG='/work/$_img'
         truncate -s \${MAKAR_DISK_MB}M \"\$IMG\"
         printf 'label: dos\nstart=2048, type=c\n' | sfdisk \"\$IMG\" >/dev/null 2>&1
         mkfs.fat -F 32 -n MAKAR --offset 2048 \"\$IMG\" >/dev/null
         echo '  FAT32 test disk ready.'"
}

# Run the black-box UI tests via QEMU HMP monitor (sendkey + screendump,
# assert on serial output).  Needs host qemu-system-i386 + nc; if either
# is missing we skip rather than fail so the wider CI suite is portable.
_run_ui_test() {
    if ! command -v qemu-system-i386 >/dev/null 2>&1; then
        echo "==> UI tests skipped (no host qemu-system-i386)"
        return 0
    fi
    if ! command -v nc >/dev/null 2>&1; then
        echo "==> UI tests skipped (no nc on PATH)"
        return 0
    fi
    if [ ! -f "$REPO_ROOT/makar.iso" ]; then
        echo "==> UI tests skipped (no makar.iso)"
        return 0
    fi
    echo "==> Running UI tests (sendkey + serial grep)..."
    ( cd "$REPO_ROOT" && bash tests/ui_test.sh "$@" )
}

# Run all phases (gdb checkpoints + bg-ktest verification + ui scenarios)
# against a single shared QEMU instance.  The kernel boots once; the
# operator can watch the whole sequence end-to-end without shutdowns
# between phases.
#
# Pipeline:
#   1. boot QEMU with -s -S (frozen, gdbstub on) + monitor socket
#   2. gdb-multiarch -batch runs tests/gdb_boot_test.py
#      -- check Multiboot2 magic, boot_checkpoints, hardware_state,
#         vesa, ktest_bg, cdrom_content.  Quits on completion; QEMU's
#         CPU resumes from wherever GDB detached.
#   3. scrape serial for "KTEST_BG: PASS" as a paranoia check
#   4. UI_REUSE_QEMU=1 hands the same QEMU to tests/ui_test.sh -- it
#      sends `verbose on` (the marker that turns serial-mirror back on
#      after the kernel's post-boot Linux-style cmdline-gated flip)
#      and runs every scenario.
#   5. HMP `quit` cleanly shuts QEMU down.
#
# GUI mode (`all graphical`) opens a visible window so the operator can
# watch the kernel transition from GDB-paused to live shell.
_run_all() {
    local _gui="${1:-0}"

    local _qemu _gdb _accel
    _qemu=$(_host_qemu)
    _gdb=$(_host_gdb)
    _accel=$(_qemu_accel)

    if [ ! -f "$REPO_ROOT/makar.iso" ]; then
        echo "ERROR: makar.iso missing -- did the build step succeed?" >&2
        return 2
    fi

    # GUI mode requires host qemu + nc (Docker can't open a display).
    # GDB is optional in GUI mode -- if host gdb-multiarch is missing we
    # skip Phase 1 and run bg-ktest + UI only.
    # Headless mode: prefer host tools; fall back to Docker for the whole
    # pipeline when any host tool is missing.
    local _docker_fallback=0
    local _skip_gdb=0
    if [ "$_gui" = "1" ]; then
        if [ -z "$_qemu" ] || ! command -v nc >/dev/null 2>&1; then
            echo "ERROR: 'all graphical' requires host qemu-system-i386 and nc." >&2
            return 2
        fi
        if [ -z "$_gdb" ]; then
            echo "WARN: host gdb-multiarch missing -- skipping Phase 1 (GDB checkpoints) in GUI mode."
            _skip_gdb=1
        fi
    else
        if [ -z "$_qemu" ] || [ -z "$_gdb" ] || ! command -v nc >/dev/null 2>&1; then
            if ! command -v "$DOCKER_BIN" >/dev/null 2>&1; then
                echo "ERROR: ./run.sh all needs either host tools (qemu+gdb+nc) or Docker." >&2
                return 2
            fi
            _docker_fallback=1
        fi
    fi

    if [ "$_docker_fallback" = "1" ]; then
        echo "==> Running unified pipeline inside Docker toolchain container..."
        _drun --as-root --env "QEMU_ACCEL=$_accel" -- \
            'bash -c "
                set -e
                command -v nc >/dev/null 2>&1 || (apt-get update -qq && apt-get install -y --no-install-recommends netcat-openbsd >/dev/null)
                rm -f /work/all-serial.log /work/all-monitor.sock /work/all-gdb.log
                echo \"==> Phase 0: starting QEMU (frozen at reset, gdbstub on :1234)...\"
                qemu-system-i386 \
                    -cdrom /work/makar.iso \
                    -m 32 -vga std -display none \
                    $QEMU_ACCEL \
                    -s -S \
                    -serial file:/work/all-serial.log \
                    -monitor unix:/work/all-monitor.sock,server,nowait \
                    -no-reboot &
                QPID=\$!
                sleep 2
                echo \"==> Phase 1: GDB boot-checkpoint suite...\"
                set +e
                timeout 300 gdb-multiarch -batch \
                    -ex \"source /work/tests/gdb_boot_test.py\" \
                    /work/src/kernel/makar.kernel 2>&1 | tee /work/all-gdb.log
                GRC=\${PIPESTATUS[0]}
                set -e
                if [ \"\$GRC\" -ne 0 ]; then
                    echo \"==> Phase 1 FAILED (rc=\$GRC)\" >&2
                    kill \"\$QPID\" 2>/dev/null || true
                    wait \"\$QPID\" 2>/dev/null || true
                    exit 1
                fi
                echo \"==> Phase 1 PASS\"
                if grep -q \"KTEST_BG: PASS\" /work/all-serial.log; then
                    echo \"==> Phase 2 PASS\"
                elif grep -q \"KTEST_BG: FAIL\" /work/all-serial.log; then
                    echo \"==> Phase 2 FAIL\" >&2
                    kill \"\$QPID\" 2>/dev/null || true
                    wait \"\$QPID\" 2>/dev/null || true
                    exit 1
                else
                    echo \"==> Phase 2 WARN (no KTEST_BG marker; Phase 1 already covered)\"
                fi
                echo \"==> Phase 3: UI scenarios (line-buffered, per-scenario PASS/FAIL streams live)...\"
                cd /work
                set +e
                UI_REUSE_QEMU=1 \
                UI_SERIAL_LOG=/work/all-serial.log \
                UI_MONITOR_SOCK=/work/all-monitor.sock \
                UI_QEMU_PID=\$QPID \
                stdbuf -oL -eL bash tests/ui_test.sh 2>&1
                URC=\$?
                set -e
                echo \"==> Phase 3 done (rc=\$URC)\"
                echo \"==> Phase 4: HMP quit...\"
                echo quit | nc -U /work/all-monitor.sock >/dev/null 2>&1 || true
                W=0
                while [ \$W -lt 50 ] && kill -0 \"\$QPID\" 2>/dev/null; do
                    sleep 0.1; W=\$((W+1))
                done
                kill -9 \"\$QPID\" 2>/dev/null || true
                wait \"\$QPID\" 2>/dev/null || true
                if [ \$URC -ne 0 ]; then
                    echo \"==> Phase 3 FAIL (rc=\$URC)\" >&2
                    exit 1
                fi
                echo \"==> ALL PHASES PASSED\"
            "'
        return $?
    fi

    local _serial="$REPO_ROOT/all-serial.log"
    local _monitor="$REPO_ROOT/all-monitor.sock"
    local _gdblog="$REPO_ROOT/all-gdb.log"
    rm -f "$_serial" "$_monitor" "$_gdblog"

    local _display="-display none"
    if [ "$_gui" = "1" ]; then
        _display="${QEMU_DISPLAY:+-display $QEMU_DISPLAY}"
    fi

    local _freeze=""
    [ "$_skip_gdb" = "0" ] && _freeze="-S"

    echo "==> Phase 0: starting QEMU ${_freeze:+(frozen at reset, gdbstub on :1234)}..."
    # shellcheck disable=SC2086
    "$_qemu" \
        -cdrom "$REPO_ROOT/makar.iso" \
        -m 32 -vga std \
        $_display \
        $_accel \
        -s $_freeze \
        -serial "file:$_serial" \
        -monitor "unix:$_monitor,server,nowait" \
        -no-reboot &
    local _qpid=$!
    sleep 2

    if [ "$_skip_gdb" = "0" ]; then
        echo "==> Phase 1: GDB boot-checkpoint suite..."
        # macOS has no `timeout(1)`; use gtimeout if present, else run raw.
        local _to=""
        if   command -v timeout  >/dev/null 2>&1; then _to="timeout 300"
        elif command -v gtimeout >/dev/null 2>&1; then _to="gtimeout 300"
        fi
        $_to "$_gdb" -batch \
            -ex "source $REPO_ROOT/tests/gdb_boot_test.py" \
            "$REPO_ROOT/src/kernel/makar.kernel" \
            2>&1 | tee "$_gdblog"
        local _gdb_rc=${PIPESTATUS[0]}
        if [ "$_gdb_rc" -ne 0 ]; then
            echo "==> Phase 1 FAILED (gdb rc=$_gdb_rc)" >&2
            kill "$_qpid" 2>/dev/null || true
            wait "$_qpid" 2>/dev/null || true
            return 1
        fi
        echo "==> Phase 1 PASS"
    else
        echo "==> Phase 1 SKIPPED (no host gdb-multiarch; GUI mode)"
        # Without GDB the kernel runs from reset normally; give it time
        # to reach the shell prompt before UI scenarios start.
        echo "==> Waiting 10s for boot-complete..."
        local _w=0
        while [ $_w -lt 60 ]; do
            grep -q "kernel: boot complete" "$_serial" 2>/dev/null && break
            sleep 0.5; _w=$((_w + 1))
        done
    fi

    echo "==> Phase 2: bg-ktest verification..."
    if grep -q "KTEST_BG: PASS" "$_serial" 2>/dev/null; then
        echo "==> Phase 2 PASS"
    elif grep -q "KTEST_BG: FAIL" "$_serial" 2>/dev/null; then
        echo "==> Phase 2 FAIL (KTEST_BG: FAIL in serial)" >&2
        kill "$_qpid" 2>/dev/null || true
        wait "$_qpid" 2>/dev/null || true
        return 1
    else
        echo "==> Phase 2 WARN (no KTEST_BG marker in serial; GDB Phase 1 already covered bg-ktest)"
    fi

    echo "==> Phase 3: UI scenarios (reusing the same QEMU)..."
    (
        cd "$REPO_ROOT"
        UI_REUSE_QEMU=1 \
        UI_SERIAL_LOG="$_serial" \
        UI_MONITOR_SOCK="$_monitor" \
        UI_QEMU_PID="$_qpid" \
        bash tests/ui_test.sh
    )
    local _ui_rc=$?

    echo "==> Phase 4: HMP quit..."
    echo quit | nc -U "$_monitor" >/dev/null 2>&1 || true
    local _waited=0
    while [ "$_waited" -lt 50 ] && kill -0 "$_qpid" 2>/dev/null; do
        sleep 0.1; _waited=$((_waited + 1))
    done
    if kill -0 "$_qpid" 2>/dev/null; then
        kill -9 "$_qpid" 2>/dev/null || true
    fi
    wait "$_qpid" 2>/dev/null || true

    if [ "$_ui_rc" -ne 0 ]; then
        echo "==> Phase 3 FAIL (ui rc=$_ui_rc)" >&2
        return 1
    fi
    echo "==> ALL PHASES PASSED"
    return 0
}

# Run the GDB ISO boot-checkpoint test.
# Boots from CD-ROM only - verifies boot sequence, background ktest, and
# CD-ROM filesystem content.
_run_gdb_iso_test() {
    echo "==> Running GDB boot tests (ISO boot)..."
    local _qemu _gdb _accel
    _qemu=$(_host_qemu)
    _gdb=$(_host_gdb)
    _accel=$(_qemu_accel)

    if [ -n "$_qemu" ] && [ -n "$_gdb" ]; then
        # shellcheck disable=SC2086
        "$_qemu" \
            -m 32 \
            -drive "file=$REPO_ROOT/makar.iso,if=ide,index=2,media=cdrom" \
            -boot order=d \
            -serial "file:$REPO_ROOT/gdb-serial.log" \
            -display none -no-reboot -no-shutdown \
            $_accel \
            -s -S &
        QPID=$!
        sleep 2
        _timeout 300 "$_gdb" -batch \
            -ex "source $REPO_ROOT/tests/gdb_boot_test.py" \
            "$REPO_ROOT/src/kernel/makar.kernel" \
            2>&1 | tee "$REPO_ROOT/gdb-test.log"
        RC=${PIPESTATUS[0]}
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        return $RC
    else
        _drun --as-root --env "QEMU_ACCEL=$_accel" -- \
            'qemu-system-i386 \
                 -drive file=/work/makar.iso,if=ide,index=2,media=cdrom \
                 -m 32 \
                 -boot order=d \
                 -serial file:/work/gdb-serial.log \
                 -display none -no-reboot -no-shutdown \
                 $QEMU_ACCEL \
                 -s -S &
             QPID=$!
             sleep 2
             timeout 300 gdb-multiarch -batch \
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
    local _qemu _gdb _accel
    _qemu=$(_host_qemu)
    _gdb=$(_host_gdb)
    _accel=$(_qemu_accel)

    if [ -n "$_qemu" ] && [ -n "$_gdb" ]; then
        # shellcheck disable=SC2086
        "$_qemu" \
            -drive "file=$REPO_ROOT/$_img,format=raw,if=ide,index=0" \
            -boot c \
            -m 32 \
            -serial "file:$REPO_ROOT/hdd-test-serial.log" \
            -display none -no-reboot -no-shutdown \
            $_accel \
            -s -S &
        QPID=$!
        sleep 2
        _timeout 120 "$_gdb" -batch \
            -ex "source $REPO_ROOT/tests/gdb_hdd_test.py" \
            "$REPO_ROOT/src/kernel/makar.kernel" \
            2>&1 | tee "$REPO_ROOT/hdd-test-gdb.log"
        RC=${PIPESTATUS[0]}
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
        return $RC
    else
        _drun --as-root --env "QEMU_ACCEL=$_accel" --env "HDD_IMG_NAME=$_img" -- \
            'qemu-system-i386 \
                 -drive file=/work/$HDD_IMG_NAME,format=raw,if=ide,index=0 \
                 -boot c \
                 -m 32 \
                 -serial file:/work/hdd-test-serial.log \
                 -display none -no-reboot -no-shutdown \
                 $QEMU_ACCEL \
                 -s -S &
             QPID=$!
             sleep 2
             timeout 120 gdb-multiarch -batch \
                 -ex "source tests/gdb_hdd_test.py" \
                 src/kernel/makar.kernel \
                 2>&1 | tee /work/hdd-test-gdb.log
             RC=${PIPESTATUS[0]}
             kill "$QPID" 2>/dev/null || true
             wait "$QPID" 2>/dev/null || true
             exit "$RC"'
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

# ── iso build ────────────────────────────────────────────────────────────────
# Incremental kernel + makar.iso + makar-test.iso build.  Used by CI's
# build job (artifacts feed the parallel ktest / gdb / ui jobs).
"iso build")
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    echo "==> Artifacts: makar.iso, makar-test.iso, src/kernel/makar.kernel"
    ;;

# ── iso boot ──────────────────────────────────────────────────────────────────
"iso boot")
    _build_iso "CFLAGS='-O0 -g3'"
    if [ ! -f "$REPO_ROOT/hdd.img" ]; then
        _drun --as-root --env "MAKAR_HDD_SIZE_MB=$MAKAR_HDD_SIZE_MB" -- \
            "qemu-img create -f raw hdd.img \${MAKAR_HDD_SIZE_MB}M"
    fi
    _run_qemu_interactive \
        "-drive file=/work/hdd.img,format=raw,if=ide,index=0 \
         -drive file=/work/makar.iso,if=ide,index=2,media=cdrom \
         -boot order=d -serial stdio"
    ;;

# ── iso test ──────────────────────────────────────────────────────────────────
# Full ISO test suite: incremental build → ktest → GDB ISO boot test → ui.
"iso test")
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_ktest
    _run_gdb_iso_test
    _run_ui_test
    echo "==> All ISO tests PASSED."
    ;;

# ── iso release ───────────────────────────────────────────────────────────────
"iso release")
    _build_iso "CFLAGS='-O2 -g'"
    echo "==> Release ISO ready: $REPO_ROOT/makar.iso"
    ;;

# ── hdd build ────────────────────────────────────────────────────────────────
"hdd build")
    _build_kernel "CFLAGS='-O0 -g3'"
    rm -f "$REPO_ROOT/$HDD_TEST_IMG"
    HDD_IMG="$HDD_TEST_IMG" HDD_SIZE_MB="$MAKAR_HDD_TEST_SIZE_MB" \
        DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    echo "==> Artifacts: $HDD_TEST_IMG (${MAKAR_HDD_TEST_SIZE_MB} MiB), src/kernel/makar.kernel"
    ;;

# ── hdd boot ──────────────────────────────────────────────────────────────────
"hdd boot")
    _build_kernel "CFLAGS='-O0 -g3'"
    rm -f "$REPO_ROOT/$HDD_IMG"
    HDD_IMG="$HDD_IMG" HDD_SIZE_MB="$MAKAR_HDD_SIZE_MB" \
        DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    _run_qemu_interactive \
        "-drive file=/work/$HDD_IMG,format=raw,if=ide,index=0 \
         -boot c -serial stdio"
    ;;

# ── hdd test ──────────────────────────────────────────────────────────────────
"hdd test")
    _build_kernel "CFLAGS='-O0 -g3'"
    if [ ! -f "$REPO_ROOT/src/kernel/makar.kernel" ]; then
        echo "ERROR: src/kernel/makar.kernel not found after build." >&2; exit 1
    fi
    rm -f "$REPO_ROOT/$HDD_TEST_IMG"
    HDD_IMG="$HDD_TEST_IMG" HDD_SIZE_MB="$MAKAR_HDD_TEST_SIZE_MB" \
        DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    _run_gdb_hdd_test "$HDD_TEST_IMG"
    echo ""
    echo "==> HDD boot test PASSED."
    echo "    GDB log:    hdd-test-gdb.log"
    echo "    Serial log: hdd-test-serial.log"
    ;;

# ── hdd release ───────────────────────────────────────────────────────────────
"hdd release")
    _build_kernel "CFLAGS='-O2 -g'"
    rm -f "$REPO_ROOT/$HDD_IMG"
    HDD_IMG="$HDD_IMG" HDD_SIZE_MB="$MAKAR_HDD_SIZE_MB" \
        DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
        "$REPO_ROOT/generate-hdd.sh"
    echo "==> HDD image ready: $REPO_ROOT/$HDD_IMG (${MAKAR_HDD_SIZE_MB} MiB)"
    ;;

# ── gdb iso ──────────────────────────────────────────────────────────────────
# GDB boot-checkpoint test against the existing ISO.  Incremental build
# first so the kernel + ISO are fresh when the gdbstub attaches.  CI's
# gdb-iso job downloads an artifact so the build step is a no-op there.
"gdb iso")
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_gdb_iso_test
    ;;

# ── gdb hdd ──────────────────────────────────────────────────────────────────
"gdb hdd")
    _build_kernel "CFLAGS='-O0 -g3'"
    if [ ! -f "$REPO_ROOT/$HDD_TEST_IMG" ]; then
        rm -f "$REPO_ROOT/$HDD_TEST_IMG"
        HDD_IMG="$HDD_TEST_IMG" DOCKER_BIN="$DOCKER_BIN" DOCKER_PLATFORM="$DOCKER_PLATFORM" \
            "$REPO_ROOT/generate-hdd.sh"
    fi
    _run_gdb_hdd_test "$HDD_TEST_IMG"
    ;;

# ── ktest (headless, default) ────────────────────────────────────────────────
# Incremental build of makar-test.iso, then run ktest against it headless.
ktest)
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_ktest
    ;;

# ── ktest graphical ──────────────────────────────────────────────────────────
# Incremental build, then run ktest in a visible QEMU window.  Requires
# host QEMU + display server.
"ktest graphical")
    QEMU_BIN=$(_host_qemu)
    if [ -z "$QEMU_BIN" ]; then
        echo "ERROR: 'ktest graphical' requires host QEMU and a display server." >&2
        echo "       Install qemu-system-i386 with X11/SDL/Cocoa support." >&2
        exit 1
    fi
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    echo "==> Running ktest suite (graphical QEMU - window closes on completion)..."
    rm -f "$REPO_ROOT/ktest.log"
    "$QEMU_BIN" \
        -cdrom "$REPO_ROOT/makar-test.iso" \
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

# ── ui (headless, default) ───────────────────────────────────────────────────
# Black-box ui-tests driven through QEMU's HMP monitor (sendkey + serial
# grep).  Requires host QEMU + nc.  Extra positional args are scenario
# filters; bare `./run.sh ui` runs the full suite.
ui)
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_ui_test "$@"
    ;;

# ── ui graphical ─────────────────────────────────────────────────────────────
# Same as `ui` but with a visible QEMU window + paced typing -- the
# debugging path when serial-only assertions aren't enough.
# QEMU_DISPLAY (cocoa|gtk|sdl) overrides QEMU's default backend pick;
# KEY_DELAY overrides the inter-keystroke pause (default 0.15 s).
"ui graphical")
    QEMU_BIN=$(_host_qemu)
    if [ -z "$QEMU_BIN" ]; then
        echo "ERROR: 'ui graphical' requires host QEMU with a display server." >&2
        echo "       Install qemu-system-i386 with X11/SDL/Cocoa support." >&2
        exit 1
    fi
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    echo "==> Running ui-tests with display window (graphical, paced typing)..."
    ( cd "$REPO_ROOT" && GUI=1 bash tests/ui_test.sh "$@" )
    ;;

# ── all (unified runner) ─────────────────────────────────────────────────────
# Boot once; run gdb checkpoints + bg-ktest scrape + ui scenarios in
# sequence against the same QEMU instance.  Keeps the separated modes
# untouched; use this when you want a single end-to-end verification.
all)
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_all 0
    ;;

# ── all graphical ────────────────────────────────────────────────────────────
"all graphical")
    _build_iso "CFLAGS='-O0 -g3' TEST_ISO=1"
    _run_all 1
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
    _usage
    ;;
esac
