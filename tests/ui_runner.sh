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
# UI_END selects what stop_qemu does at the end of a GUI run:
#   shutdown (default) - type `shutdown`, kernel ACPI-offs, QEMU exits.
#   reboot             - type `reboot`, then LEAVE QEMU running so you can
#                        manually boot the freshly installed internal drive
#                        from GRUB's "next available device" entry.  Used to
#                        verify an `install` end-to-end.  No -no-reboot, so the
#                        guest actually resets into GRUB instead of exiting.
#   stay               - send nothing; LEAVE QEMU running at the live shell
#                        prompt so you can poke around by hand (e.g. inspect
#                        /log).  Blocks until you close the window / Ctrl-C.
UI_END=${UI_END:-shutdown}
if [ "$GUI" = "1" ]; then
    # Pick a sane default display backend per OS so `ui graphical` opens
    # a visible window without the caller having to set QEMU_DISPLAY.
    # macOS: cocoa; Linux: gtk; fallback: sdl.  Override with QEMU_DISPLAY.
    if [ -z "${QEMU_DISPLAY:-}" ]; then
        case "$(uname -s)" in
            Darwin)  QEMU_DISPLAY=cocoa ;;
            Linux)   QEMU_DISPLAY=gtk ;;
            *)       QEMU_DISPLAY=sdl ;;
        esac
    fi
    DISPLAY_ARG="-display $QEMU_DISPLAY"
    # 0.4 s/key in GUI mode: the visible window + TCG can lag the PS/2 IRQ ->
    # ring -> shell_readline pipeline, and a too-fast burst drops characters or
    # races a VT switch.  Override with KEY_DELAY=… for a specific run.
    KEY_DELAY=${KEY_DELAY:-0.4}
    if [ "$UI_END" = "reboot" ]; then
        REBOOT_ARG=""
    else
        REBOOT_ARG="-no-reboot"
    fi
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
    echo "ui_test: $ISO not found - build first with './run.sh iso build'" >&2
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
    LOGDIR=$(mktemp -d -t makar-ui.XXXXXX)
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

# Canonical VFS path roots -- single source of truth so scenarios never
# hand-spell a path.  The rootfs (CD-ROM on live boot, ext2/FAT32 on HDD
# boot) is elevated to "/", so /apps, /usr, /etc, /home, /root resolve
# transparently regardless of which medium is active.  Compose app paths
# as "$P_APPS/foo.elf"; /mnt/cdrom and /mnt/hd are deprecated -- the only
# explicit /mnt path retained is $P_MNT itself, for `ls /mnt` coverage.
P_PROC=/proc
P_DEV=/dev
P_MNT=/mnt
P_APPS=/apps

# keys "STRING" -- emit one `sendkey <name>` line per character of STRING,
# translating punctuation to QEMU HMP key names.  No trailing Enter, so
# callers append `sendkey ret` themselves.  This is the ONE place that knows
# how to type text: scenarios write `$(keys "exec $P_APPS/hello.elf")`
# instead of a 25-line hand-expanded sendkey block.  Feed the result to
# `it` or `send_script` (both consume newline-separated `sendkey` lines).
keys() {
    local s=$1 i c
    for (( i = 0; i < ${#s}; i++ )); do
        c=${s:i:1}
        case "$c" in
            ' ')  echo "sendkey spc" ;;
            '/')  echo "sendkey slash" ;;
            '.')  echo "sendkey dot" ;;
            '-')  echo "sendkey minus" ;;
            '_')  echo "sendkey shift-minus" ;;
            '=')  echo "sendkey equal" ;;
            '+')  echo "sendkey shift-equal" ;;
            ',')  echo "sendkey comma" ;;
            ';')  echo "sendkey semicolon" ;;
            ':')  echo "sendkey shift-semicolon" ;;
            '"')  echo "sendkey shift-apostrophe" ;;
            "'")  echo "sendkey apostrophe" ;;
            '(')  echo "sendkey shift-9" ;;
            ')')  echo "sendkey shift-0" ;;
            '*')  echo "sendkey shift-8" ;;
            '?')  echo "sendkey shift-slash" ;;
            '!')  echo "sendkey shift-1" ;;
            '$')  echo "sendkey shift-4" ;;
            '%')  echo "sendkey shift-5" ;;
            '[')  echo "sendkey bracket_left" ;;
            ']')  echo "sendkey bracket_right" ;;
            [a-z0-9]) echo "sendkey $c" ;;
            [A-Z]) echo "sendkey shift-$(printf '%s' "$c" | tr '[:upper:]' '[:lower:]')" ;;
            *)    echo "sendkey $c" ;;   # last-ditch; QEMU may reject
        esac
    done
}

# --- QEMU lifecycle ---------------------------------------------------------

stop_qemu() {
    # Unified-runner mode: the caller owns QEMU.  Don't touch it.
    if [ "${UI_REUSE_QEMU:-0}" = "1" ]; then
        return 0
    fi

    if [ -z "$QEMU_PID" ] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        return 0
    fi

    if [ "$GUI" = "1" ] && [ "$UI_END" = "reboot" ]; then
        # Reboot mode: type `reboot` and hand the window back to the operator.
        # The guest resets into GRUB (no -no-reboot) where you can pick
        # "next available device" to boot the just-installed internal drive.
        # We do NOT kill QEMU - block until you close the window yourself.
        sleep 1
        send_script 'sendkey r
sendkey e
sendkey b
sendkey o
sendkey o
sendkey t
sendkey ret'
        echo "UI_END=reboot: guest rebooting into GRUB; pick 'next available"
        echo "  device' to boot the installed drive.  Close the QEMU window"
        echo "  (or Ctrl-C here) when done."
        wait "$QEMU_PID" 2>/dev/null
        return 0
    elif [ "$GUI" = "1" ] && [ "$UI_END" = "stay" ]; then
        # Stay mode: tests are done; leave the guest at its live shell prompt
        # so the operator can drive it by hand.  No keystrokes, no kill -- block
        # until the window is closed (or Ctrl-C here).
        echo "UI_END=stay: tests done; QEMU left running at the shell."
        echo "  Poke around (e.g. 'ls /log', 'cat /log/kernel.log'); close the"
        echo "  QEMU window (or Ctrl-C here) when done."
        wait "$QEMU_PID" 2>/dev/null
        return 0
    elif [ "$GUI" = "1" ]; then
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
    # Unified-runner mode: a QEMU instance is already running, owned by
    # the caller (`./run.sh all`), with monitor + serial paths already
    # set up.  Skip the boot wait and the launch -- jump straight to
    # the verbose-on handshake.  The caller owns shutdown.
    if [ "${UI_REUSE_QEMU:-0}" = "1" ]; then
        SERIAL_LOG="${UI_SERIAL_LOG:?UI_SERIAL_LOG required when UI_REUSE_QEMU=1}"
        MONITOR_SOCK="${UI_MONITOR_SOCK:?UI_MONITOR_SOCK required when UI_REUSE_QEMU=1}"
        QEMU_PID="${UI_QEMU_PID:-0}"
        local waited=0
        while [ $waited -lt 30 ] && [ ! -S "$MONITOR_SOCK" ]; do
            sleep 0.2; waited=$((waited + 1))
        done
        if [ ! -S "$MONITOR_SOCK" ]; then
            echo "FAIL: monitor socket $MONITOR_SOCK not present" >&2
            return 1
        fi
        # Reuse mode: GDB just detached.  Sleep so shell0 reaches its
        # prompt before keystrokes (the [shell:ready vt=N] marker is
        # gated by g_serial_verbose which is OFF post-boot, so we can't
        # sync on it -- wall-clock instead).
        echo "  reuse-mode: waiting 8s for shell to reach prompt post-detach..."
        sleep 8
        local _dumpdir="${UI_TEST_LOGDIR:-/work}"
        echo "  reuse-mode: screendump -> $_dumpdir/all-postdetach.ppm"
        echo "screendump $_dumpdir/all-postdetach.ppm" | nc -U "$MONITOR_SOCK" >/dev/null 2>&1 || true
        sleep 0.5
        echo "  reuse-mode: wakeup newline"
        send_script 'sendkey ret'
        sleep 1
        echo "  reuse-mode: screendump -> $_dumpdir/all-after-wakeup.ppm"
        echo "screendump $_dumpdir/all-after-wakeup.ppm" | nc -U "$MONITOR_SOCK" >/dev/null 2>&1 || true
        sleep 0.5
        echo "  reuse-mode: sending verbose on"
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
        sleep 1.5
        echo "  reuse-mode: screendump -> $_dumpdir/all-after-verbose.ppm"
        echo "screendump $_dumpdir/all-after-verbose.ppm" | nc -U "$MONITOR_SOCK" >/dev/null 2>&1 || true
        sleep 0.5
        echo "  reuse-mode: serial tail after verbose-on:"
        if [ -f "$SERIAL_LOG" ]; then
            local _bytes
            _bytes=$(wc -c < "$SERIAL_LOG")
            local _from=$(( _bytes > 512 ? _bytes - 512 : 0 ))
            dd if="$SERIAL_LOG" bs=1 skip="$_from" 2>/dev/null | sed 's/^/    | /'
        fi
        return 0
    fi

    rm -f "$SERIAL_LOG" "$MONITOR_SOCK"

    # Blank scratch disk so /dev/hda exists for the mount-workflow scenario.
    # It carries no partition table (auto-mount is FAT32-only, so it stays
    # unmounted at boot); the scenario formats it with mkfs.ext2 and mounts
    # it itself.  Lives in LOGDIR so it's cleaned up with everything else.
    SCRATCH_HDD="$LOGDIR/scratch-hda.img"
    # The scratch /dev/hda must fit a release install (FAT32 minimum 33 MiB
    # bootfs + ext2 rootfs holding apps + src + docs).  Use the release
    # sizing (default 256 MiB) so the install scenario works; smaller
    # scenarios (filetest, mnt-mountpoint) are unaffected by the slack.
    dd if=/dev/zero of="$SCRATCH_HDD" bs=1M count="${MAKAR_HDD_SIZE_MB:-256}" 2>/dev/null

    # Boot order: `once=d` boots the CD-ROM on the FIRST boot (the live system
    # that runs the installer); after a guest-initiated reboot QEMU falls back
    # to `order=c` and boots the internal disk - so an `install` followed by
    # reboot lands in the freshly written limine MBR instead of the CD again.
    # `-net none` removes any NIC so a failed disk boot can't fall through to
    # PXE/network ROM.
    # shellcheck disable=SC2086
    "$QEMU" \
        -cdrom "$ISO" \
        -drive file="$SCRATCH_HDD",format=raw,if=ide,index=0,media=disk \
        -boot once=d,order=c \
        -net none \
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
    #
    # CRITICAL TIMING: the shell sits in a `while (!ktest_bg_done)
    # task_yield()` loop until background ktests finish (several minutes
    # under TCG).  Once ktest_bg sets the done flag, the shell drains
    # the keyboard ring (`while (keyboard_poll())`), tossing any earlier
    # keystrokes -- so `verbose on` typed pre-spinner ends up eaten.
    # Wait for the "KTEST_BG: PASS" marker before typing.
    local bg_timeout=${UI_BG_KTEST_TIMEOUT:-300}
    waited=0
    while [ $waited -lt $((bg_timeout * 2)) ]; do
        if grep -q "KTEST_BG: PASS\|KTEST_BG: FAIL" "$SERIAL_LOG" 2>/dev/null; then break; fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "FAIL: QEMU exited before KTEST_BG completed" >&2
            return 1
        fi
        sleep 0.5
        waited=$((waited + 1))
    done
    # Brief grace for the shell to actually finish its drain + first
    # prompt repaint.
    sleep 2
    # A wakeup <ret> first absorbs any first-post-boot keystroke-loss
    # race -- without it the leading `v` of `verbose on` lands during
    # the loading-screen-to-prompt handoff and gets dropped, leaving
    # `erbose on` which makbox rejects.
    send_script 'sendkey ret'
    sleep 0.6
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
# Previously fenced the next test off from the prior one with a
# Ctrl+C anchor.  That was needed before per-task exec_params closed
# the static-argv race; today a half-second settle is sufficient and
# avoids spurious "^C" bytes landing in the serial mirror that some
# assertions then trip on.  Tests that explicitly exercise Ctrl+C
# (calc.elf abort, tab-cycle abort) still send it inline.
# The first call after boot is a no-op: nothing has run yet, the shell
# is freshly prompted, and the anchor would only burn ~0.6 s of visible
# typing for no semantic effect.  Subsequent calls do the full anchor
# so prior-test state never leaks into the next one.
RESET_SHELL_CALLED=0
reset_shell() {
    if [ "$RESET_SHELL_CALLED" = "0" ]; then
        RESET_SHELL_CALLED=1
        return 0
    fi
    send_script 'sendkey alt-f1'
    sleep 0.5
    # Defensive escape from any nested shell a prior failing test left
    # running (notably sh.elf).  Typing `exit<Enter>` exits sh.elf cleanly;
    # in the kernel shell it surfaces as "Unknown command 'exit'" which is
    # harmless noise.  The extra settle covers the [sys_exit] / [reaper]
    # serial burst after sh.elf reaps -- without it the next typed
    # keystroke gets eaten by the reaper-output race.
    send_script 'sendkey e
sendkey x
sendkey i
sendkey t
sendkey ret'
    sleep 0.8
    send_script 'sendkey c
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

# expect_key <pattern> <start_bytes> <sendkey-script> [timeout_s=8]
#   The "wait for the screen, then act" idiom for driving a multi-screen TUI
#   deterministically: poll the serial slice (from <start_bytes>) until
#   <pattern> appears, then send <sendkey-script>.  Returns 1 and flags the
#   test as failed if the marker never shows.  Re-syncing on each screen's
#   marker means there is no cumulative pause drift (the flake class that made
#   the old fixed-PAUSE installer scenario unreliable under TCG).
#
#   The brief settle after the marker lets the screen's getkey() register its
#   keyboard consumer before we type, so the keystroke lands in its ring
#   rather than racing the marker print.
expect_key() {
    local pattern=$1 start=$2 script=$3 timeout=${4:-8}
    if ! wait_for_serial "$pattern" "$start" "$timeout"; then
        echo "  - expect_key: timed out waiting for: $pattern"
        CURRENT_FAILED=1
        return 1
    fi
    sleep "${EXPECT_SETTLE:-1.0}"
    send_script "$script"
    return 0
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

# Render the current serial slice for inline failure context: compact
# whitespace, cap at 800 bytes (long enough to see what actually
# happened, short enough not to drown the failure summary).
_assert_render_serial() {
    if [ ! -s "$CURRENT_SEGMENT" ]; then
        echo "(empty)"
        return
    fi
    # Use Read-via-shell rather than cat to keep this hook-friendly:
    # awk reads the file, normalises whitespace, prints up to 800 bytes.
    awk 'BEGIN { RS=""; ORS="" } { gsub(/\r/, ""); gsub(/[ \t]+/, " "); print }' \
        "$CURRENT_SEGMENT" \
        | awk -v max=800 '{
            if (length($0) > max) {
                print substr($0, 1, max) " ...[truncated " (length($0) - max) " bytes]"
            } else { print }
          }'
}

# assert_serial_contains <needle>... -- every needle must appear as a
# fixed-string substring of the current test's serial slice.  On failure
# prints each missing needle alongside a snippet of what *was* in serial
# so the operator can see the divergence without opening the log file.
assert_serial_contains() {
    local missing=()
    for needle in "$@"; do
        if ! grep -qF -- "$needle" "$CURRENT_SEGMENT"; then
            missing+=("$needle")
        fi
    done
    if [ ${#missing[@]} -ne 0 ]; then
        CURRENT_FAILED=1
        local got
        got=$(_assert_render_serial)
        for needle in "${missing[@]}"; do
            echo "  - expected (missing): \"$needle\""
        done
        echo "  - got: $got"
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
        for needle in "${present[@]}"; do
            echo "  - unexpected (forbidden but present): \"$needle\""
        done
        local got
        got=$(_assert_render_serial)
        echo "  - got: $got"
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
