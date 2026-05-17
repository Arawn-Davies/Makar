#!/usr/bin/env bash
# ui_runner.sh -- shared test-runner framework for the UI test suite.
#
# Sourced (not executed) by tests/ui_test.sh.  Provides:
#   start_qemu / stop_qemu  -- boot one VM, share it across all tests
#   it <name> <script>      -- reset, drive keystrokes, capture serial slice
#   assert_serial_contains  -- positive substring assertions
#   assert_serial_not_contains  -- negative substring assertions
#   run_test <name>         -- dispatch test_<name>, report PASS/FAIL
#
# Test files (e.g. ui_tests.sh) define test_<name>() functions that call
# `it` once and then issue one or more assert_* checks.

set -u

# --- Environment / config ---------------------------------------------------

ISO=${ISO:-makar.iso}
QEMU=${QEMU:-qemu-system-i386}

# Per-keystroke pacing.  In headless mode the kernel under TCG can't always
# keep up with a fully bursted sendkey stream - characters get dropped or
# arrive after a VT switch has fired, which scrambles prompts.  30 ms/key
# is plenty for the PS/2 IRQ + ring + shell_readline pipeline to drain and
# is still 8-10x faster than GUI mode.
GUI=${GUI:-0}
if [ "$GUI" = "1" ]; then
    DISPLAY_ARG=${QEMU_DISPLAY:+-display $QEMU_DISPLAY}
    KEY_DELAY=${KEY_DELAY:-0.15}
    REBOOT_ARG="-no-reboot"
else
    DISPLAY_ARG="-display none"
    KEY_DELAY=${KEY_DELAY:-0.03}
    REBOOT_ARG="-no-reboot -no-shutdown"
fi

# Time `it` waits after the script finishes typing before snapshotting the
# serial slice.  Most commands complete in <500 ms; calc and exec-heavy
# tests can override via `it <name> <script> <wait_secs>`.
IT_DEFAULT_WAIT=${IT_DEFAULT_WAIT:-1.2}

# --- Sanity checks -----------------------------------------------------------

if [ ! -f "$ISO" ]; then
    echo "ui_test: $ISO not found - build first with './run.sh iso-build'" >&2
    exit 2
fi
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "ui_test: $QEMU not on PATH; this target needs host qemu" >&2
    exit 2
fi

# --- Log dir ----------------------------------------------------------------

if [ -n "${UI_TEST_LOGDIR:-}" ]; then
    LOGDIR=$UI_TEST_LOGDIR
    mkdir -p "$LOGDIR"
else
    LOGDIR=$(mktemp -d -t makar-ui)
    trap 'rm -rf "$LOGDIR"' EXIT
fi

SERIAL_LOG="$LOGDIR/ui.serial"
MONITOR_SOCK="$LOGDIR/ui.mon"
QEMU_PID=""

# --- Sendkey helpers --------------------------------------------------------

# send_script -- feed the HMP monitor a multi-line sendkey script.  Paces
# each keystroke with KEY_DELAY so the kernel has time to drain its PS/2
# ring + shell readline buffer between keys.
#
# A line of the form `PAUSE <secs>` is treated as an inline sleep rather
# than a sendkey - useful when a child task (calc, vix, ...) takes a moment
# to be ready for input after launch, so subsequent keys aren't lost.
send_script() {
    local script=$1
    local paced=1
    if [ "$KEY_DELAY" = "0" ] || [ -z "$KEY_DELAY" ]; then
        paced=0
    fi
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        if [[ "$line" == PAUSE* ]]; then
            local secs=${line#PAUSE }
            sleep "$secs"
            continue
        fi
        echo "$line" | nc -U "$MONITOR_SOCK" >/dev/null
        [ "$paced" = "1" ] && sleep "$KEY_DELAY"
    done <<< "$script"
}

# --- QEMU lifecycle ---------------------------------------------------------

stop_qemu() {
    if [ -z "$QEMU_PID" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        return 0
    fi

    if [ "$GUI" = "1" ]; then
        # GUI mode drives a real kernel shutdown (acpi -> port 0x604) so
        # the watcher sees a "Shutting down..." final frame.
        sleep 1
        send_script 'sendkey s
sendkey h
sendkey u
sendkey t
sendkey d
sendkey o
sendkey w
sendkey n
sendkey ret'
        local waited=0
        while [ $waited -lt 80 ] && kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 0.1; waited=$((waited + 1))
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "WARN: kernel did not ACPI-off; sending HMP quit" >&2
            echo "quit" | nc -U "$MONITOR_SOCK" >/dev/null 2>&1 || true
            waited=0
            while [ $waited -lt 50 ] && kill -0 "$QEMU_PID" 2>/dev/null; do
                sleep 0.1; waited=$((waited + 1))
            done
        fi
    else
        echo "quit" | nc -U "$MONITOR_SOCK" >/dev/null 2>&1 || true
        local waited=0
        while [ $waited -lt 50 ] && kill -0 "$QEMU_PID" 2>/dev/null; do
            sleep 0.1; waited=$((waited + 1))
        done
    fi

    if kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "WARN: QEMU did not exit on shutdown; killing" >&2
        kill -9 "$QEMU_PID" 2>/dev/null
    fi
    wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""
}

start_qemu() {
    rm -f "$SERIAL_LOG" "$MONITOR_SOCK"

    # shellcheck disable=SC2086
    "$QEMU" \
        -cdrom "$ISO" \
        -m 256 \
        -vga std \
        $DISPLAY_ARG \
        $REBOOT_ARG \
        -serial "file:$SERIAL_LOG" \
        -monitor "unix:$MONITOR_SOCK,server,nowait" \
        &
    QEMU_PID=$!
    trap 'stop_qemu' EXIT

    # Boot-complete marker.  TCG under CI containers can take 30-60 s;
    # bump UI_BOOT_TIMEOUT if you see false fails.
    local boot_timeout=${UI_BOOT_TIMEOUT:-90}
    local waited=0
    local max_ticks=$((boot_timeout * 2))
    while [ $waited -lt $max_ticks ]; do
        if grep -q "kernel: boot complete" "$SERIAL_LOG" 2>/dev/null; then break; fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "FAIL: QEMU exited before boot-complete" >&2
            return 1
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
    if [ $waited -ge $max_ticks ]; then
        echo "FAIL: boot-complete marker never appeared (waited ${boot_timeout}s)" >&2
        return 1
    fi
    sleep 1

    waited=0
    while [ $waited -lt 30 ] && [ ! -S "$MONITOR_SOCK" ]; do
        sleep 0.2; waited=$((waited + 1))
    done
    if [ ! -S "$MONITOR_SOCK" ]; then
        echo "FAIL: monitor socket never appeared" >&2
        return 1
    fi

    # Mirror shell output to COM1 for the rest of the session.  The flag
    # is sticky so we only set it once for the whole shared-VM run.
    send_script 'sendkey v
sendkey e
sendkey r
sendkey b
sendkey o
sendkey s
sendkey e
sendkey spc
sendkey o
sendkey n
sendkey ret'
    sleep 0.8
}

# --- Reset shell between tests ----------------------------------------------

# reset_shell -- return the focused shell to a known anchor (VT0, cwd=/).
# Ctrl+C aborts any running command or clears partial input; `cd /` then
# anchors cwd regardless of where prior tests left us.  alt-f1 first so
# any Alt+Fn excursion is undone before we type.
#
# Used to need two Ctrl+Cs as a timing fence - in shared-VM runs the
# next test's `cd /` would otherwise race the previous exec's
# shutdown and stomp the static argv globals in shell_exec_elf.
# Per-task exec_params (task_t.exec_params) closed that race at the
# kernel level, so one Ctrl+C is enough now.
reset_shell() {
    send_script 'sendkey alt-f1
sendkey ctrl-c
sendkey c
sendkey d
sendkey spc
sendkey slash
sendkey ret'
    sleep 0.6
}

# --- Test primitives --------------------------------------------------------

CURRENT_NAME=""
CURRENT_SEGMENT=""
CURRENT_DUMP=""
CURRENT_FAILED=0

# wait_for_serial <pattern> <start_bytes> [timeout_s=5]
#   Polls $SERIAL_LOG for the first match of <pattern> in the slice
#   starting at byte offset <start_bytes>.  Returns 0 when found, 1 on
#   timeout.  Used by `it_until` (and by tests directly) to replace
#   fixed sleeps with "sync on a marker" -- the expect/pexpect idiom.
wait_for_serial() {
    local pattern=$1
    local start_bytes=$2
    local timeout=${3:-5}
    local deadline=$(($(date +%s) + timeout))
    while [ $(date +%s) -lt $deadline ]; do
        if [ -f "$SERIAL_LOG" ] && \
           dd if="$SERIAL_LOG" bs=1 skip="$start_bytes" 2>/dev/null \
              | grep -qE -- "$pattern"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# it_until <label> <sendkey-script> <sync-pattern> [timeout_s=5]
#   Like `it`, but instead of sleeping a fixed duration after the keys
#   are sent, polls the serial log until <sync-pattern> appears (or
#   the timeout elapses).  The shell's `shell_readline` emits
#   `[shell:ready vt=N]` on entry, so `'\[shell:ready vt=0\]'` is the
#   natural sync marker for "the shell is back at the prompt".
it_until() {
    CURRENT_NAME=$1
    local script=$2
    local pattern=$3
    local timeout=${4:-5}

    reset_shell

    local start_bytes=0
    if [ -f "$SERIAL_LOG" ]; then
        start_bytes=$(wc -c < "$SERIAL_LOG")
    fi

    send_script "$script"

    if ! wait_for_serial "$pattern" "$start_bytes" "$timeout"; then
        echo "  - wait_for_serial timed out waiting for: $pattern"
    fi

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2

    dd if="$SERIAL_LOG" bs=1 skip="$start_bytes" 2>/dev/null > "$CURRENT_SEGMENT"
}

# it <label> <sendkey-script> [extra-wait-secs]
#   Resets the shell, drives the keystrokes, waits long enough for the
#   command(s) to complete, snapshots the per-test serial slice into
#   $CURRENT_SEGMENT, and takes a screendump for triage.
it() {
    CURRENT_NAME=$1
    local script=$2
    local wait_secs=${3:-$IT_DEFAULT_WAIT}

    reset_shell

    local start_bytes=0
    if [ -f "$SERIAL_LOG" ]; then
        start_bytes=$(wc -c < "$SERIAL_LOG")
    fi

    send_script "$script"
    sleep "$wait_secs"

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2

    dd if="$SERIAL_LOG" bs=1 skip="$start_bytes" 2>/dev/null > "$CURRENT_SEGMENT"
}

# assert_serial_contains <needle>... -- every needle must appear as a
# fixed-string substring of the current test's serial slice.
assert_serial_contains() {
    local missing=()
    for needle in "$@"; do
        if ! grep -qF -- "$needle" "$CURRENT_SEGMENT"; then
            missing+=("$needle")
        fi
    done
    if [ ${#missing[@]} -ne 0 ]; then
        CURRENT_FAILED=1
        echo "  - missing in serial: ${missing[*]}"
    fi
}

# assert_serial_not_contains <needle>... -- none of the needles may appear.
assert_serial_not_contains() {
    local present=()
    for needle in "$@"; do
        if grep -qF -- "$needle" "$CURRENT_SEGMENT"; then
            present+=("$needle")
        fi
    done
    if [ ${#present[@]} -ne 0 ]; then
        CURRENT_FAILED=1
        echo "  - unexpectedly present in serial: ${present[*]}"
    fi
}

# run_test <name> -- dispatch test_<name>, report PASS/FAIL using the
# accumulated assert results.  Returns non-zero iff the test failed.
run_test() {
    local name=$1
    CURRENT_FAILED=0
    "test_${name}"
    if [ $CURRENT_FAILED -eq 0 ]; then
        echo "PASS [$CURRENT_NAME]"
        return 0
    fi
    echo "FAIL [$CURRENT_NAME]"
    echo "       serial: $CURRENT_SEGMENT"
    echo "       dump:   $CURRENT_DUMP"
    return 1
}
