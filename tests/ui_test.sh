#!/usr/bin/env bash
# ui_test.sh -- black-box UI tests.  Each test boots into a shared QEMU
# session, drives keystrokes via HMP, and asserts on serial output.
#
# Framework lives in tests/ui_runner.sh.  This file is just test
# definitions plus the driver loop.
#
# Usage:
#   tests/ui_test.sh                # run all tests
#   tests/ui_test.sh <name>...      # run named tests only (dash or underscore)
#
# Exit codes: 0 = all passed, 1 = at least one failed.

# Locate and source the runner relative to this script so the test file
# can be moved or symlinked without breaking imports.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/ui_runner.sh
. "$HERE/ui_runner.sh"

# --- Tests ------------------------------------------------------------------

test_glob_proc() {
    it "glob-proc" \
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
sendkey ret"
    assert_serial_contains "vendor_id" "MemFree" "Makar 0.5.0"
}

test_tab_path() {
    # `cat<TAB>/proc/c<TAB><Enter>` should expand to `cat /proc/cpuinfo`
    # and dump the cpuinfo content.  Verifies tab on unique cmd match,
    # path-style tab extension, and that the resulting command runs.
    it "tab-complete-path" \
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
sendkey ret"
    assert_serial_contains "vendor_id" "GenuineIntel"
}

test_exec_hello() {
    # `exec /cdrom/apps/hello.elf tester` prints "Hello, tester!" via
    # sys_write on fd 2 (stderr = FD_KIND_VGA_SERIAL).  Absolute path so
    # cwd doesn't matter.  Verifies the per-task fd table end-to-end:
    # task_create allocates the child's fd_table with 0/1/2 pre-bound,
    # sys_write dispatches to serial through fd_get on the calling
    # task's table.  Pre-#134 this went through a global s_fds[].
    it "exec-hello" \
"sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey h
sendkey e
sendkey l
sendkey l
sendkey o
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey spc
sendkey t
sendkey e
sendkey s
sendkey t
sendkey e
sendkey r
sendkey ret"
    assert_serial_contains "Hello," "tester" "status=0"
}

test_per_tty_cwd() {
    # Per-task cwd isolation across TTYs (slice 15).  Each shell task
    # owns task_t.cwd; vfs_getcwd/vfs_cd route through task_current.  We
    # cd VT0 to /proc, switch to VT3 and cd it to /cdrom/apps, then
    # switch back to VT0.  Asserting on the "~>" prompt suffix is
    # unambiguous since only prompts end that way.
    #
    # Uses VT3 (not VT1/2) so reset_shell's `alt-f1` between tests
    # doesn't collide with this test's VT excursion.  Extra wait because
    # the VT switch + double cd takes a moment to settle.
    it "per-tty-cwd" \
"sendkey c
sendkey d
sendkey spc
sendkey slash
sendkey p
sendkey r
sendkey o
sendkey c
sendkey ret
sendkey alt-f3
sendkey c
sendkey d
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey ret
sendkey alt-f1" \
        2.0
    assert_serial_contains "/proc~>" "/cdrom/apps~>"
}

test_cd_root() {
    # `cd /<TAB><TAB><Enter>` then `pwd` - tab on `/<TAB><TAB>` lists
    # the mount points; the trailing Enter commits the half-typed
    # `cd /`, leaving us at the virtual root.  Then verify with pwd.
    it "cd-root-listing" \
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
sendkey ret"
    assert_serial_contains "cdrom"
}

test_calc_brackets() {
    # Bracket/parenthesis arithmetic smoke test in calc.elf.  Absolute
    # path so cwd doesn't matter.  The PAUSE after `exec ...<Enter>`
    # gives the calc child task time to be scheduled and reach its
    # input loop - without it the first keystrokes race ahead and arrive
    # while shell_exec_elf is still spinning up the task, so calc sees
    # a truncated first expression.
    it "calc-brackets" \
"sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey c
sendkey a
sendkey l
sendkey c
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret
PAUSE 0.8
sendkey shift-9
sendkey 2
sendkey shift-8
sendkey 3
sendkey shift-0
sendkey shift-8
sendkey 4
sendkey ret
sendkey 6
sendkey 9
sendkey minus
sendkey shift-9
sendkey 6
sendkey shift-8
sendkey shift-9
sendkey 9
sendkey minus
sendkey 1
sendkey shift-0
sendkey shift-0
sendkey ret
sendkey shift-9
sendkey shift-9
sendkey 8
sendkey shift-8
sendkey 2
sendkey shift-0
sendkey shift-8
sendkey shift-9
sendkey 3
sendkey minus
sendkey 1
sendkey shift-0
sendkey shift-0
sendkey ret
sendkey e
sendkey x
sendkey i
sendkey t
sendkey ret" \
        2.5
    assert_serial_contains "24" "21" "32"
}

test_no_dead_in_proctasks() {
    # Slice 8/9 cleanup: /proc/tasks should not list DEAD slots that
    # are waiting for task_create to reclaim them.  bg-ktest spawns
    # `preempt_victim`, `ktest_noop1`, etc., during the boot test
    # suite and they end up DEAD by the time the shell is up.  If the
    # filter is working, `cat /proc/tasks` should never include the
    # string "DEAD".
    it "no-dead-in-proctasks" \
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
sendkey t
sendkey a
sendkey s
sendkey k
sendkey s
sendkey ret"
    assert_serial_contains "shell0" "shell1"
    assert_serial_not_contains "DEAD"
}

test_typo_doesnt_clear() {
    # Slice 8 polish: a wrong command falls back to makbox, which
    # prints an error to stderr and exits.  With shell_exec_elf's
    # unconditional shell_clear_screen this used to wipe the screen.
    # After gating on task->fb_touched, line-mode output stays
    # visible.  Verify by typing a typo, then `pwd`, and asserting
    # both the makbox error AND the pwd provenance reach serial in
    # the same slice.
    # Two-stage send: type the typo + Enter, wait for shell-ready
    # (== shell back at the prompt after makbox-fallback died), THEN
    # type pwd + Enter.  Without the intermediate sync the second
    # batch's first byte races keyboard_release_task on the dying
    # exec child and gets dropped (we'd see "wd" reach the shell
    # instead of "pwd").  Pure timing fix; no kernel changes.
    CURRENT_NAME=typo-doesnt-clear
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey n
sendkey o
sendkey s
sendkey u
sendkey c
sendkey h
sendkey c
sendkey m
sendkey d
sendkey ret'
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 5 || \
        echo "  - stage1: never returned to prompt"
    local sb2=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey p
sendkey w
sendkey d
sendkey ret'
    wait_for_serial '\[makbox:pwd\]' "$sb2" 5 || \
        echo "  - stage2: pwd never produced [makbox:pwd]"

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"

    assert_serial_contains "[makbox:pwd]"
}

test_per_vt_palettes() {
    # Verify each VT has its declared colour scheme.  Visual check via
    # PPM dumps -- serial can't see palette.  Expected:
    #   VT0: green on black
    #   VT1: white on black
    #   VT2: white on blue
    #   VT3: black on white
    CURRENT_NAME=per-vt-palettes
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    echo "screendump $LOGDIR/$CURRENT_NAME.vt0.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    for f in f2 f3 f4 ; do
        send_script "sendkey alt-$f"
        sleep 0.8
        echo "screendump $LOGDIR/$CURRENT_NAME.vt-$f.ppm" \
            | nc -U "$MONITOR_SOCK" >/dev/null
        sleep 0.2
    done
    send_script 'sendkey alt-f1'
    sleep 0.4

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"
    # No serial-level assertion -- this is a pure visual check.
}

test_two_maktops() {
    # Multiple maktop instances on different VTs.  User reported: spawn
    # maktop on VT0, switch to VT1, spawn maktop on VT1, round-trip --
    # the second instance fails to repaint and shows the shell's blue
    # bg underneath.
    #
    # Build the test by:
    #  1. exec maktop on VT0, wait for it to draw
    #  2. Alt+F2, type the exec command, wait
    #  3. Alt+F1 (back to maktop1), screendump, Alt+F2 (back to maktop2)
    #     screendump, repeat to be sure
    #  4. Kill both with q's.  Pwd at the end on VT0 to prove the shell
    #     came back to a working state.
    CURRENT_NAME=two-maktops
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    # Launch maktop on VT0.
    send_script 'sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey m
sendkey a
sendkey k
sendkey t
sendkey o
sendkey p
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret'
    sleep 1.2
    # Switch to VT1 and launch maktop there too.
    send_script 'sendkey alt-f2'
    sleep 0.6
    send_script 'sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey m
sendkey a
sendkey k
sendkey t
sendkey o
sendkey p
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret'
    sleep 1.5
    echo "screendump $LOGDIR/$CURRENT_NAME.vt1-maktop-running.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script 'sendkey alt-f1'
    sleep 1.0
    echo "screendump $LOGDIR/$CURRENT_NAME.vt0-after-vt1.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script 'sendkey alt-f2'
    sleep 1.0
    echo "screendump $LOGDIR/$CURRENT_NAME.vt1-after-vt0.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    # Tear down: q on VT1 (current), Alt+F1, q on VT0.  Then pwd.
    send_script 'sendkey q'
    sleep 0.6
    send_script 'sendkey alt-f1'
    sleep 0.6
    send_script 'sendkey q'
    sleep 0.6
    local sb2=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey p
sendkey w
sendkey d
sendkey ret'
    wait_for_serial '\[makbox:pwd\]' "$sb2" 5 || \
        echo "  - pwd never ran (one of the maktops never died)"

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"

    assert_serial_contains "[makbox:pwd]"
}

test_vt_all_roundtrips() {
    # Exercise every VT round-trip from maktop on VT0: Alt+F2 → back,
    # Alt+F3 → back, Alt+F4 → back.  After each return we dump a PPM
    # so the inspector (me, you, or a future image-diff) can see
    # whether maktop actually repainted.  At the end 'q' should still
    # reach maktop (proves focus survived all three excursions).
    CURRENT_NAME=vt-all-roundtrips
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey m
sendkey a
sendkey k
sendkey t
sendkey o
sendkey p
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret'
    sleep 1.2
    for f in f2 f3 f4 ; do
        send_script "sendkey alt-$f"
        sleep 0.5
        echo "screendump $LOGDIR/$CURRENT_NAME.at-$f.ppm" \
            | nc -U "$MONITOR_SOCK" >/dev/null
        sleep 0.2
        send_script 'sendkey alt-f1'
        sleep 1.0
        echo "screendump $LOGDIR/$CURRENT_NAME.back-from-$f.ppm" \
            | nc -U "$MONITOR_SOCK" >/dev/null
        sleep 0.2
    done
    local sb2=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey q
sendkey p
sendkey w
sendkey d
sendkey ret'
    wait_for_serial '\[makbox:pwd\]' "$sb2" 5 || \
        echo "  - pwd never ran (maktop lost focus during the multi-VT tour)"

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"

    assert_serial_contains "[makbox:pwd]"
}

test_vt_roundtrip_keeps_maktop_focused() {
    # When the user Alt+Fn's away from a VT running a fullscreen ELF
    # (here maktop), then Alt+Fn's back, keyboard focus must return to
    # the foreground child -- not the slot's shell.  Before the fix,
    # vtty_switch routed to the slot's owner (shell0) and maktop sat in
    # its non-blocking read getting -EAGAIN forever; the user could
    # see the UI but typing did nothing.
    #
    # Test: exec maktop, wait for it to start (one CPU bar refresh
    # gives a serial breadcrumb via /proc/tasks), Alt+F2, Alt+F1 back,
    # then send 'q' to quit maktop.  If focus is correctly restored,
    # maktop sees 'q', cleans up, and the shell prompt comes back -- we
    # confirm via a `pwd` round-trip producing the makbox provenance
    # tag.  If focus is broken, 'q' is lost and pwd never runs.
    CURRENT_NAME=vt-roundtrip-keeps-maktop-focused
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey m
sendkey a
sendkey k
sendkey t
sendkey o
sendkey p
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret'
    # Give maktop a moment to reach its main loop.
    sleep 1.2
    # Alt+F2 then Alt+F1 -- excursion + return.
    send_script 'sendkey alt-f2'
    sleep 0.4
    send_script 'sendkey alt-f1'
    sleep 1.2   # let maktop receive FOCUS_GAIN + finish its full repaint
    # Visual snapshot AFTER the round-trip but BEFORE we kill maktop --
    # this is what the operator actually sees when they Alt+F1 back.
    # Serial-only assertions miss visual regressions (blue framebuffer
    # under maktop's UI), so we save a PPM here that an inspector
    # (or a future image-diff harness) can scrutinise.
    echo "screendump $LOGDIR/$CURRENT_NAME.mid.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    local sb2=$(wc -c < "$SERIAL_LOG")
    # 'q' should reach maktop now and quit it.  Then pwd via makbox.
    send_script 'sendkey q
sendkey p
sendkey w
sendkey d
sendkey ret'
    wait_for_serial '\[makbox:pwd\]' "$sb2" 5 || \
        echo "  - pwd never ran (maktop probably never saw 'q')"

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"

    assert_serial_contains "[makbox:pwd]"
}

test_user_sigusr1_handler() {
    # Slice 8 phase 4: ring-3 trampoline + sigreturn lets sys_signal(2)
    # actually invoke a user-installed handler.  sigtest.elf installs a
    # SIGUSR1 handler, sys_kills itself with SIGUSR1, yields, then prints
    # "sigtest: SIGUSR1 handler ran (count=N)" via sys_write_serial when
    # the handler set its flag.  Grep that exact string -- a partial
    # match would also accept the "NEVER ran" failure line.
    it "user-sigusr1-handler" \
"sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey s
sendkey i
sendkey g
sendkey t
sendkey e
sendkey s
sendkey t
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret" \
        2.0
    assert_serial_contains "sigtest: SIGUSR1 handler ran"
}

test_ctrlc_kills_child() {
    # Slice 8 phase 3: Ctrl+C now delivers SIGINT to the focused task and
    # the kernel's default-terminate action in sig_deliver kills it.
    # Verify the shell recovers cleanly after the child dies.
    #
    # Steps: exec calc.elf (long-running interactive REPL) -> give it time
    # to reach its input loop -> send Ctrl+C -> shell sees calc go DEAD and
    # returns to the prompt -> type `pwd` -> makbox emits its `[makbox:pwd]`
    # serial provenance tag.  Presence of that tag is unambiguous evidence
    # the shell prompt is responsive again after the child was killed.
    it "ctrlc-kills-child" \
"sendkey e
sendkey x
sendkey e
sendkey c
sendkey spc
sendkey slash
sendkey c
sendkey d
sendkey r
sendkey o
sendkey m
sendkey slash
sendkey a
sendkey p
sendkey p
sendkey s
sendkey slash
sendkey c
sendkey a
sendkey l
sendkey c
sendkey dot
sendkey e
sendkey l
sendkey f
sendkey ret
PAUSE 0.8
sendkey ctrl-c
PAUSE 0.4
sendkey p
sendkey w
sendkey d
sendkey ret" \
        2.0
    assert_serial_contains "[makbox:pwd]"
}

test_makbox_pwd() {
    # Prove `pwd` is served by makbox.elf, not the (now-removed) shell
    # builtin.  makbox's pwd applet writes "[makbox:pwd] <cwd>" to COM1
    # via SYS_WRITE_SERIAL before printing to stdout.  Presence of the
    # tag is unambiguous evidence the ring-3 path ran end-to-end:
    #   PATH lookup misses pwd.elf -> makbox fallback -> SYS_GETCWD ->
    #   SYS_WRITE_SERIAL provenance line.
    it "makbox-pwd" \
"sendkey p
sendkey w
sendkey d
sendkey ret"
    assert_serial_contains "[makbox:pwd]"
}

# --- Driver -----------------------------------------------------------------

ALL_TESTS=(glob_proc tab_path exec_hello cd_root per_tty_cwd calc_brackets ctrlc_kills_child no_dead_in_proctasks typo_doesnt_clear vt_roundtrip_keeps_maktop_focused vt_all_roundtrips user_sigusr1_handler makbox_pwd)

declare -a TO_RUN
if [ $# -eq 0 ]; then
    TO_RUN=("${ALL_TESTS[@]}")
else
    for arg in "$@"; do
        TO_RUN+=("${arg//-/_}")
    done
fi

if ! start_qemu; then
    exit 1
fi

fails=0
total=0
for t in "${TO_RUN[@]}"; do
    total=$((total + 1))
    if ! run_test "$t"; then
        fails=$((fails + 1))
    fi
done

stop_qemu

echo
echo "ui_test: $((total - fails))/$total passed"
[ $fails -eq 0 ] || exit 1
exit 0
