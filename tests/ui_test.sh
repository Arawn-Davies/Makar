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

ALL_TESTS=(glob_proc tab_path exec_hello cd_root per_tty_cwd calc_brackets ctrlc_kills_child makbox_pwd)

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
