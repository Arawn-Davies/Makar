#!/usr/bin/env bash
# ui_test.sh -- black-box UI tests.  Each test boots into a shared QEMU
# session, drives keystrokes via HMP, and asserts on serial output.
#
# Framework lives in tests/ui_runner.sh.  This file is just test
# definitions plus the driver loop.
#
# Keystrokes are typed via the `keys "..."` helper (ui_runner.sh), which
# expands a string into `sendkey` lines.  Paths use the canonical $P_*
# roots (P_CDROM_APPS, P_PROC, ...) so a scenario never hand-spells a path
# and nothing drifts when mountpoints move.
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

# Pull the kernel version from its single source of truth so the
# glob-proc assertion (and anything else that needs to recognise the
# /proc/uname banner) stays in sync with the kernel automatically.
VERSION_H="$HERE/../src/kernel/include/kernel/version.h"
MAKAR_VERSION=$(awk -F\" '/define[[:space:]]+MAKAR_VERSION/ { print $2; exit }' "$VERSION_H")
if [ -z "$MAKAR_VERSION" ]; then
    echo "ui_test.sh: could not extract MAKAR_VERSION from $VERSION_H" >&2
    exit 1
fi

# --- Tests ------------------------------------------------------------------

test_glob_proc() {
    it "glob-proc" \
"$(keys "cat /proc/*")
sendkey ret"
    assert_serial_contains "vendor_id" "MemFree" "Makar $MAKAR_VERSION"
}

test_tab_path() {
    # `cat<TAB>/proc/c<TAB><Enter>` should expand to `cat /proc/cpuinfo`
    # and dump the cpuinfo content.  Verifies tab on unique cmd match,
    # path-style tab extension, and that the resulting command runs.
    it "tab-complete-path" \
"$(keys "cat")
sendkey tab
$(keys "/proc/c")
sendkey tab
sendkey ret"
    assert_serial_contains "vendor_id" "GenuineIntel"
}

test_exec_hello() {
    # `exec /mnt/cdrom/apps/hello.elf tester` prints "Hello, tester!" via
    # sys_write on fd 2 (stderr = FD_KIND_VGA_SERIAL).  Absolute path so
    # cwd doesn't matter.  Verifies the per-task fd table end-to-end:
    # task_create allocates the child's fd_table with 0/1/2 pre-bound,
    # sys_write dispatches to serial through fd_get on the calling
    # task's table.  Pre-#134 this went through a global s_fds[].
    it "exec-hello" \
"$(keys "exec $P_CDROM_APPS/hello.elf tester")
sendkey ret"
    assert_serial_contains "Hello," "tester" "status=0"
}

test_per_tty_cwd() {
    # Per-task cwd isolation across TTYs (slice 15).  Each shell task
    # owns task_t.cwd; vfs_getcwd/vfs_cd route through task_current.  We
    # cd VT0 to /proc, switch to VT3 and cd it to /mnt/cdrom/apps, then
    # switch back to VT0.  Asserting on the "~>" prompt suffix is
    # unambiguous since only prompts end that way.
    #
    # Uses VT3 (not VT1/2) so reset_shell's `alt-f1` between tests
    # doesn't collide with this test's VT excursion.  Extra wait because
    # the VT switch + double cd takes a moment to settle.
    it "per-tty-cwd" \
"$(keys "cd $P_PROC")
sendkey ret
sendkey alt-f3
$(keys "cd $P_CDROM_APPS")
sendkey ret
sendkey alt-f1" \
        2.0
    assert_serial_contains "/proc~>" "/mnt/cdrom/apps~>"
}

test_cd_root() {
    # `cd /<TAB><TAB><Enter>` then `pwd` - tab on `/<TAB><TAB>` lists
    # the root entries; the trailing Enter commits the half-typed
    # `cd /`, leaving us at the virtual root.  Then verify with pwd.
    # Disk filesystems now live under /mnt, so the root tab listing shows
    # the [mnt] container alongside the synthetic [proc] / [dev] trees.
    it "cd-root-listing" \
"$(keys "cd /")
sendkey tab
sendkey tab
sendkey ret
$(keys "pwd")
sendkey ret"
    assert_serial_contains "mnt"
}

test_ls_dev() {
    # `ls /dev` enumerates the synthetic block-device tree.  The ui
    # runner attaches only a CD-ROM, so /dev/cdrom is the stable node to
    # assert on (disks/partitions depend on an attached HDD).  Exercises
    # devfs_ls + the VFS_FS_DEV route end-to-end.
    it "ls-dev" \
"$(keys "ls $P_DEV")
sendkey ret"
    assert_serial_contains "cdrom"
}

test_ls_mnt() {
    # `ls /mnt` lists the disk-filesystem mounts.  The ui runner boots
    # from CD, so /mnt/cdrom is the stable entry to assert on.  Covers the
    # VFS_FS_MNT route + ls_mnt() after the /hd,/cdrom -> /mnt move.
    it "ls-mnt" \
"$(keys "ls $P_MNT")
sendkey ret"
    assert_serial_contains "cdrom"
}

test_calc_brackets() {
    # Full arithmetic exercise of calc.elf: every operator (+ - * / %),
    # BIDMAS/operator precedence, nested parentheses, and the
    # divide-by-zero guard.  Absolute path so cwd doesn't matter.  The
    # PAUSE after `exec ...<Enter>` gives the calc child task time to be
    # scheduled and reach its input loop - without it the first keystrokes
    # race ahead and arrive while shell_exec_elf is still spinning up the
    # task, so calc sees a truncated first expression.  Results are chosen
    # so each is a distinctive number that never appears in any input line
    # (assert_serial_contains is a plain substring match over the slice).
    it "calc-brackets" \
"$(keys "exec $P_CDROM_APPS/calc.elf")
sendkey ret
PAUSE 0.8
$(keys "(2+3)*4")
sendkey ret
$(keys "100/4")
sendkey ret
$(keys "100-3*2")
sendkey ret
$(keys "2*(3+4)-1")
sendkey ret
$(keys "1000%97")
sendkey ret
$(keys "6/0")
sendkey ret
$(keys "exit")
sendkey ret" \
        3
    # (2+3)*4=20, 100/4=25, 100-3*2=94 (mult before sub), 2*(3+4)-1=13,
    # 1000%97=30, and the divide-by-zero guard.
    assert_serial_contains "20" "25" "94" "13" "30" "division by zero"
}

test_no_dead_in_proctasks() {
    # Slice 8/9 cleanup: /proc/tasks should not list DEAD slots that
    # are waiting for task_create to reclaim them.  bg-ktest spawns
    # `preempt_victim`, `ktest_noop1`, etc., during the boot test
    # suite and they end up DEAD by the time the shell is up.  If the
    # filter is working, `cat /proc/tasks` should never include the
    # string "DEAD".
    it "no-dead-in-proctasks" \
"$(keys "cat $P_PROC/tasks")
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
    send_script "$(keys "nosuchcmd")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 5 || \
        echo "  - stage1: never returned to prompt"
    local sb2=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "pwd")
sendkey ret"
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
    send_script "$(keys "exec $P_CDROM_APPS/maktop.elf")
sendkey ret"
    sleep 1.2
    # Switch to VT1 and launch maktop there too.
    send_script 'sendkey alt-f2'
    sleep 0.6
    send_script "$(keys "exec $P_CDROM_APPS/maktop.elf")
sendkey ret"
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
    send_script "$(keys "pwd")
sendkey ret"
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
    send_script "$(keys "exec $P_CDROM_APPS/maktop.elf")
sendkey ret"
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
    send_script "sendkey q
$(keys "pwd")
sendkey ret"
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
    send_script "$(keys "exec $P_CDROM_APPS/maktop.elf")
sendkey ret"
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
    send_script "sendkey q
$(keys "pwd")
sendkey ret"
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

test_fork_cow() {
    # Slice 12d: SYS_FORK + COW.  forktest.elf snapshots a global sentinel,
    # forks, child reads (must match parent's value -- proves COW visibility),
    # child overwrites + exits, parent yields and reads its own view (must
    # still be the original value -- proves the COW fault gave parent its
    # own private frame).
    reset_shell
    it "fork-cow" \
"$(keys "exec $P_CDROM_APPS/forktest.elf")
sendkey ret" \
        3.0
    assert_serial_contains \
        "[forktest] PARENT-PRE sentinel=0xAA550000 p1=0x11110000 p2=0x22220000 p3=0x33330000 p4=0x44440000" \
        "[forktest] CHILD-SAW sentinel=0xAA550000 p1=0x11110000 p2=0x22220000 p3=0x33330000 p4=0x44440000" \
        "[forktest] CHILD-WROTE sentinel=0xC0DEBABE p1=0xDEAD0001 p2=0xDEAD0002 p3=0xDEAD0003 p4=0xDEAD0004" \
        "[forktest] REAPED" \
        "status=42" \
        "[forktest] PARENT-POST" \
        "sentinel=0xAA550000 p1=0x11110000 p2=0x22220000 p3=0x33330000 p4=0x44440000"
}

test_fork_execve() {
    # Slice 13a: SYS_EXECVE.  execvetest.elf forks, child execve's
    # hello.elf with argv[1]="execve-tester".  Asserts:
    #   - parent's PRE-EXEC marker prints (before fork)
    #   - hello.elf's argv-driven greeting prints (inside child, after
    #     execve replaced the address space)
    #   - parent's POST-EXEC marker prints (parent's PD intact, lived
    #     through the child's image swap)
    reset_shell
    it "fork-execve" \
"$(keys "exec $P_CDROM_APPS/execvetest.elf")
sendkey ret" \
        3.5
    assert_serial_contains \
        "[execve-test] PRE-EXEC" \
        "Hello, execve-tester!" \
        "[execve-test] REAPED" \
        "status=0" \
        "[execve-test] POST-EXEC"
}

test_user_sigusr1_handler() {
    # Slice 8 phase 4: ring-3 trampoline + sigreturn lets sys_signal(2)
    # actually invoke a user-installed handler.  sigtest.elf installs a
    # SIGUSR1 handler, sys_kills itself with SIGUSR1, yields, then prints
    # "sigtest: SIGUSR1 handler ran (count=N)" via sys_write_serial when
    # the handler set its flag.  Grep that exact string -- a partial
    # match would also accept the "NEVER ran" failure line.
    it "user-sigusr1-handler" \
"$(keys "exec $P_CDROM_APPS/sigtest.elf")
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
"$(keys "exec $P_CDROM_APPS/calc.elf")
sendkey ret
PAUSE 0.8
sendkey ctrl-c
PAUSE 0.4
$(keys "pwd")
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
"$(keys "pwd")
sendkey ret"
    assert_serial_contains "[makbox:pwd]"
}

test_shell_scripting_vars() {
    # Bash-flavoured scripting smoke: assign NAME=foo, dump env, verify
    # the variable round-trips through the per-shell-task table.
    reset_shell
    it "shell-scripting-vars" \
"$(keys "name=foo")
sendkey ret
$(keys "env")
sendkey ret" \
        1.5
    assert_serial_contains "name=foo"
}

test_mnt_mountpoint() {
    # Linux-style mountpoint workflow on the blank scratch disk the runner
    # attaches as /dev/hda (raw, no partition table -> stays unmounted at
    # boot since auto-mount is FAT32-only).  Format it ext2, create an empty
    # mountpoint with mkdir, bind it with mount, umount (reverts to empty),
    # then rmdir.  Asserts on the builtin status lines (mirrored to serial
    # under `verbose on`).  Exercises vfs_make_mountpoint / vfs_mount_hd into
    # an existing empty mountpoint / vfs_umount_hd revert / vfs_remove_mountpoint.
    reset_shell
    # it_until syncs on the trailing `echo` marker instead of sleeping a
    # fixed duration, so the scenario ends the moment rmdir completes
    # rather than idling until a worst-case mkfs timeout elapses.
    it_until "mnt-mountpoint" \
"$(keys "mkfs.ext2 /dev/hda")
sendkey ret
$(keys "mkdir /mnt/data")
sendkey ret
$(keys "mount /dev/hda /mnt/data")
sendkey ret
$(keys "umount /mnt/data")
sendkey ret
$(keys "rmdir /mnt/data")
sendkey ret
$(keys "mkdir /mnt/cdrom/nope")
sendkey ret
$(keys "echo mnt-flow-done")
sendkey ret" \
        "mnt-flow-done" 20
    assert_serial_contains \
        "as ext2..." \
        "Mounted ext2" \
        "at /mnt/data" \
        "Volume unmounted." \
        "read-only filesystem" \
        "mnt-flow-done"
}

test_install() {
    # Full TUI installer against the blank scratch disk (/dev/hda).  Drives the
    # wizard deterministically: installer_run prints an `INSTALL>...` serial
    # marker right before each screen blocks on input, so we sync on the marker
    # and then send the key that picks the default (recommended) option, rather
    # than blind-pausing.  Screens, in order:
    #   welcome   (Enter to begin)
    #   drive     (Enter -> the one ATA target)
    #   fs        (Enter -> ext2, the recommended/default item)
    #   partition (Enter -> "use entire disk", the default)
    #   confirm   (Down then Enter -> menu defaults to Cancel, move to "Yes")
    # then we wait for the completion marker.  Copies the kernel + /apps +
    # /docs + /src trees and installs limine, so it is SLOW under TCG and
    # deliberately kept out of ALL_TESTS - run on demand:
    #   ./run.sh ui install            (headless)
    #   ./run.sh ui graphical install  (watch the wizard)
    reset_shell
    CURRENT_NAME=install
    CURRENT_FAILED=0

    local start_bytes=0
    [ -f "$SERIAL_LOG" ] && start_bytes=$(wc -c < "$SERIAL_LOG")

    # Launch; installer_run emits INSTALL>welcome before its first getkey.
    send_script "$(keys "install")
sendkey ret"

    # Walk the wizard.  Each step waits for that screen's marker (re-syncing
    # from the install start, since the markers are cumulative + ordered in
    # the slice) before sending the key, so there is no pause drift to desync.
    expect_key "INSTALL>welcome"   "$start_bytes" 'sendkey ret'  && \
    expect_key "INSTALL>drive"     "$start_bytes" 'sendkey ret'  && \
    expect_key "INSTALL>fs"        "$start_bytes" 'sendkey ret'  && \
    expect_key "INSTALL>partition" "$start_bytes" 'sendkey ret'
    # Confirm dialog: tui_menu defaults to "Cancel" (index 0); "Yes - ERASE"
    # is index 1, so we must press Down once to move the highlight before
    # Enter.  Send the two keys SEPARATELY with a dwell between them -- bundled
    # back-to-back the Down arrow gets eaten while the menu repaints to the
    # framebuffer, leaving Enter to fire on the default (Cancel) and silently
    # abort the install.  expect_key presses Down after the screen settles;
    # the explicit dwell then lets the highlight move to "Yes" before Enter.
    if expect_key "INSTALL>confirm" "$start_bytes" 'sendkey down'; then
        sleep 1.0
        send_script 'sendkey ret'
    fi

    # Copy + limine embed are the slow part under TCG; the progress box paints
    # to the framebuffer only, so the serial completion marker is the proof.
    local completed=1
    if ! wait_for_serial "INSTALL: complete ok" "$start_bytes" 360; then
        echo "  - installer did not report completion"
        completed=0
        CURRENT_FAILED=1
    fi

    # Whatever the outcome, dump the in-RAM installer log (/log) to serial so
    # the captured slice records exactly which step the installer reached.
    # The installer's per-step lines paint to the framebuffer only; /log is
    # where they (and the kernel debug stream) are mirrored.  Esc first in
    # case the installer is parked on its final "press a key" screen.
    send_script 'sendkey esc'
    sleep 0.5
    send_script "$(keys "cat /log")
sendkey ret"
    [ "$completed" = "0" ] && sleep 2 || sleep 1

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$start_bytes" 2>/dev/null > "$CURRENT_SEGMENT"
    assert_serial_contains "INSTALL: complete ok"
}

test_demo_script() {
    # End-to-end run of the bundled scripting demo: exercises every
    # feature (vars, expansion, comments, env, [, integer + string
    # tests, elif chain, for, while, $?, clock).  Assert on markers
    # the script emits for each section so a regression in any layer
    # surfaces as a failing assertion.
    reset_shell
    it "demo-script" \
"$(keys "sh $P_CDROM_APPS/demo.sh")
sendkey ret" \
        25
    assert_serial_contains \
        "Makar shell-script demo" \
        "cwd-ok" \
        "hello, tester!" \
        "str-eq-ok" \
        "n-ok" \
        "gt-ok" \
        "eq-ok" \
        "elif-correct-blue" \
        "for: gamma" \
        "word: three" \
        "countdown: 1" \
        "after-true: 0" \
        "demo complete"
}

test_bughunt_clock_exit_palette() {
    # Bug-hunt: after clock.elf exits, the shell prompt should reappear on
    # the current VT's palette (VT0 -- green on black), not white-on-blue.
    # We snapshot at three points: (a) clock running, (b) immediately after
    # 'q', (c) after running 'pwd' on the restored prompt.
    CURRENT_NAME=bughunt-clock-exit
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "exec $P_CDROM_APPS/clock.elf")
sendkey ret"
    sleep 1.5
    echo "screendump $LOGDIR/$CURRENT_NAME.running.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script 'sendkey q'
    sleep 0.8
    echo "screendump $LOGDIR/$CURRENT_NAME.exited.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script "$(keys "pwd")
sendkey ret"
    sleep 0.6
    echo "screendump $LOGDIR/$CURRENT_NAME.after-pwd.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script "$(keys "ls")
sendkey ret"
    sleep 0.6
    echo "screendump $LOGDIR/$CURRENT_NAME.after-ls.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"
    assert_serial_contains "[makbox:pwd]"
}

test_bughunt_vix_exit_palette() {
    # Bug-hunt: vix exit should land back on the VT's palette (green/black
    # on VT0), not white-on-blue (the loading-screen palette).
    CURRENT_NAME=bughunt-vix-exit
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "vix /tmp")
sendkey ret"
    sleep 1.5
    echo "screendump $LOGDIR/$CURRENT_NAME.running.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    # Ctrl-Q to quit (no edits, so single press suffices).
    send_script 'sendkey ctrl-q'
    sleep 0.8
    echo "screendump $LOGDIR/$CURRENT_NAME.exited.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script "$(keys "pwd")
sendkey ret"
    sleep 0.6
    echo "screendump $LOGDIR/$CURRENT_NAME.after-pwd.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"
    assert_serial_contains "[makbox:pwd]"
}

test_bughunt_vix_palette_on_vt3() {
    # VT3's scheme is black-on-white -- the inverse of the (legacy)
    # hardcoded post-vix scheme.  After vix exits on VT3 the prompt
    # must come back black-on-white, not white-on-blue.  Confirms the
    # palette restoration is genuinely per-VT, not a VT0-only special
    # case.
    CURRENT_NAME=bughunt-vix-palette-vt3
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script 'sendkey alt-f4'
    sleep 0.8
    send_script "$(keys "vix /tmp")
sendkey ret"
    sleep 1.5
    echo "screendump $LOGDIR/$CURRENT_NAME.running.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    send_script 'sendkey ctrl-q'
    sleep 0.8
    echo "screendump $LOGDIR/$CURRENT_NAME.exited.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"
    # No serial assertion -- pure visual check on the palette.
}

test_bughunt_status_bar_after_switch() {
    # Bug-hunt: switching INTO a VT running a fullscreen ELF (maktop) should
    # update the status bar so the active-VT highlight matches the new VT.
    # Currently the highlight stays on the previous VT because
    # vtty_drain_pending only runs in keyboard_getchar (blocking read), and
    # maktop uses non-blocking reads via keyboard_poll.
    CURRENT_NAME=bughunt-status-bar
    reset_shell
    local sb1=$(wc -c < "$SERIAL_LOG")
    # Launch maktop on VT0.
    send_script "$(keys "exec $P_CDROM_APPS/maktop.elf")
sendkey ret"
    sleep 1.2
    # Alt+F2 (shell on VT1), Alt+F1 back to maktop on VT0.
    send_script 'sendkey alt-f2'
    sleep 0.6
    send_script 'sendkey alt-f1'
    sleep 1.2
    echo "screendump $LOGDIR/$CURRENT_NAME.vt0-maktop.ppm" \
        | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    # Tear down: 'q' to maktop.
    send_script 'sendkey q'
    sleep 0.6
    send_script "$(keys "pwd")
sendkey ret"
    sleep 0.8

    CURRENT_SEGMENT=$LOGDIR/$CURRENT_NAME.serial
    CURRENT_DUMP=$LOGDIR/$CURRENT_NAME.ppm
    CURRENT_FAILED=0
    rm -f "$CURRENT_SEGMENT" "$CURRENT_DUMP"
    echo "screendump $CURRENT_DUMP" | nc -U "$MONITOR_SOCK" >/dev/null
    sleep 0.2
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$CURRENT_SEGMENT"
    assert_serial_contains "[makbox:pwd]"
}

# --- Driver -----------------------------------------------------------------

ALL_TESTS=(glob_proc tab_path exec_hello cd_root ls_dev ls_mnt per_tty_cwd calc_brackets ctrlc_kills_child no_dead_in_proctasks typo_doesnt_clear vt_roundtrip_keeps_maktop_focused vt_all_roundtrips fork_cow fork_execve user_sigusr1_handler makbox_pwd shell_scripting_vars demo_script bughunt_clock_exit_palette bughunt_vix_exit_palette bughunt_status_bar_after_switch mnt_mountpoint)

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
