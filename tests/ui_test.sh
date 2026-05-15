#!/usr/bin/env bash
# ui_test.sh - black-box UI tests driven through QEMU's HMP monitor.
#
# Each scenario boots the test ISO headless, feeds keyboard input via
# `sendkey` over the monitor socket, and asserts on substrings in the
# kernel's serial log (mirror of every t_writestring call).  No PPM
# parsing - the serial mirror catches everything any command echoes.
# Cases that depend on visual-only state (cursor position, gutter
# rendering) are out of scope; manual sendkey + screendump for those.
#
# Usage:
#   tests/ui_test.sh                # run all scenarios
#   tests/ui_test.sh <name>...      # run named scenarios only
#
# Exit codes: 0 = all passed, 1 = at least one failed.

set -u

ISO=${ISO:-makar.iso}
if [ ! -f "$ISO" ]; then
    echo "ui_test: $ISO not found - build first with './run.sh iso-build'" >&2
    exit 2
fi

QEMU=${QEMU:-qemu-system-i386}
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "ui_test: $QEMU not on PATH; this target needs host qemu" >&2
    exit 2
fi

# Allow CI to capture logs by passing $UI_TEST_LOGDIR; otherwise use a
# scratch dir and clean up on exit.
if [ -n "${UI_TEST_LOGDIR:-}" ]; then
    LOGDIR=$UI_TEST_LOGDIR
    mkdir -p "$LOGDIR"
else
    LOGDIR=$(mktemp -d -t makar-ui)
    trap 'rm -rf "$LOGDIR"' EXIT
fi

run_scenario() {
    local name=$1
    local script=$2
    shift 2
    local expects=("$@")

    local serial=$LOGDIR/$name.serial
    local mon=$LOGDIR/$name.mon
    local dump=$LOGDIR/$name.ppm

    rm -f "$serial" "$mon" "$dump"

    # -no-reboot + -no-shutdown keep a triple fault or kernel panic from
    # turning into an infinite reboot loop that hangs CI.
    "$QEMU" \
        -cdrom "$ISO" \
        -m 256 \
        -vga std \
        -display none \
        -no-reboot -no-shutdown \
        -serial "file:$serial" \
        -monitor "unix:$mon,server,nowait" \
        &
    local pid=$!

    # Wait up to ${BOOT_TIMEOUT:-90}s for "kernel: boot complete".  TCG
    # under CI containers can take 30-60s to reach this marker on its
    # own; bump UI_BOOT_TIMEOUT in the workflow if you see false fails.
    local boot_timeout=${UI_BOOT_TIMEOUT:-90}
    local waited=0
    local max_ticks=$((boot_timeout * 2))   # 0.5s ticks
    while [ $waited -lt $max_ticks ]; do
        if grep -q "kernel: boot complete" "$serial" 2>/dev/null; then break; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "FAIL [$name]: QEMU exited before boot-complete" >&2
            return 1
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
    if [ $waited -ge $max_ticks ]; then
        echo "FAIL [$name]: boot-complete marker never appeared (waited ${boot_timeout}s)" >&2
        kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
        return 1
    fi
    # Small grace period for the shell tasks (created after the marker)
    # to register their kbd slots and print their prompts.
    sleep 1

    # Wait for monitor socket.
    waited=0
    while [ $waited -lt 30 ] && [ ! -S "$mon" ]; do
        sleep 0.2
        waited=$((waited + 1))
    done

    # Prepend `verbose on` so the shell mirrors output to COM1 for the
    # duration of the test.  By default (Linux-style) the shell only
    # writes to the framebuffer, which the serial log wouldn't see.
    # The verbose toggle itself writes its confirmation to serial only
    # AFTER the flag flips, so we can also assert that the toggle ran.
    local preamble='sendkey v
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
    nc -U "$mon" <<< "$preamble" >/dev/null
    sleep 0.8

    # Drive scenario keystrokes.
    nc -U "$mon" <<< "$script" >/dev/null

    # Let commands drain.  Three seconds covers any current scenario;
    # bump if a test starts running anything slow.
    sleep 3

    # Snapshot the screen (handy for triage; not asserted on here).
    echo "screendump $dump" | nc -U "$mon" >/dev/null
    sleep 0.3
    echo "quit" | nc -U "$mon" >/dev/null

    # Bounded shutdown.  If QEMU doesn't honour the HMP `quit` within
    # 5s (monitor socket lost, kernel wedged, etc.) escalate to SIGKILL
    # so a single hang can't burn the whole CI minute budget.
    local exit_waited=0
    while [ $exit_waited -lt 50 ] && kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        exit_waited=$((exit_waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "WARN [$name]: QEMU did not exit on quit; killing" >&2
        kill -9 "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null

    # Assert every expected substring appears in serial.
    local missing=()
    for needle in "${expects[@]}"; do
        if ! grep -qF -- "$needle" "$serial"; then
            missing+=("$needle")
        fi
    done

    if [ ${#missing[@]} -eq 0 ]; then
        echo "PASS [$name]"
        return 0
    else
        echo "FAIL [$name]: missing in serial: ${missing[*]}"
        echo "       serial: $serial"
        echo "       dump:   $dump"
        return 1
    fi
}

# --- Scenarios ---------------------------------------------------------------
#
# Each scenario is named; selecting a subset on the command line filters by
# name.  Scripts are HMP `sendkey` commands; the framework appends a `ret` if
# the script doesn't already end with one - reduces boilerplate.

scenario_glob_proc() {
    run_scenario "glob-proc" \
"sendkey c
sendkey a
sendkey t
sendkey spc
sendkey slash
sendkey p
sendkey r
sendkey o
sendkey c
sendkey slash
sendkey shift-8
sendkey ret" \
        "vendor_id" "MemFree" "Makar 0.5.0"
}

scenario_tab_path() {
    # `cat<TAB>/proc/c<TAB><Enter>` should expand to `cat /proc/cpuinfo`
    # and dump the cpuinfo content.  Verifies: visible-feedback tab on
    # unique cmd match (`cat ` with trailing space), path-style tab
    # completion extension, and that the resulting command runs.
    run_scenario "tab-complete-path" \
"sendkey c
sendkey a
sendkey t
sendkey tab
sendkey slash
sendkey p
sendkey r
sendkey o
sendkey c
sendkey slash
sendkey c
sendkey tab
sendkey ret" \
        "vendor_id" "GenuineIntel"
}

scenario_cd_root() {
    # `cd /<TAB><TAB><Enter>` then `pwd` - tab on /<TAB><TAB> lists the
    # mount points; the trailing Enter commits the half-typed `cd /`,
    # leaving us at the virtual root.  Then verify with pwd.
    run_scenario "cd-root-listing" \
"sendkey c
sendkey d
sendkey spc
sendkey slash
sendkey tab
sendkey tab
sendkey ret
sendkey p
sendkey w
sendkey d
sendkey ret" \
        "cdrom"
}

# --- Driver ------------------------------------------------------------------

ALL_SCENARIOS=(glob_proc tab_path cd_root)

declare -a TO_RUN
if [ $# -eq 0 ]; then
    TO_RUN=("${ALL_SCENARIOS[@]}")
else
    for arg in "$@"; do
        TO_RUN+=("${arg//-/_}")
    done
fi

fails=0
total=0
for s in "${TO_RUN[@]}"; do
    total=$((total + 1))
    if ! "scenario_${s}"; then
        fails=$((fails + 1))
    fi
done

echo
echo "ui_test: $((total - fails))/$total passed"
[ $fails -eq 0 ] || exit 1
exit 0
