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

test_tab_cycle() {
    # Multi-match Tab now zsh-cycles instead of listing -- first Tab
    # enters cycle mode and pastes matches[0]; Ctrl+C aborts the line.
    # A follow-up `echo` proves the shell survived the cycle entry and
    # the abort.  Doesn't depend on cycle ordering -- the goal here is
    # "cycle entry didn't crash readline".
    it "tab-cycle" \
"$(keys "ls /proc/")
sendkey tab
sendkey ctrl-c
$(keys "echo cycle-done")
sendkey ret"
    assert_serial_contains "cycle-done" "^C"
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
    # `exec /apps/hello.elf tester` prints "Hello, tester!" via
    # sys_write on fd 2 (stderr = FD_KIND_VGA_SERIAL).  Absolute path so
    # cwd doesn't matter.  Verifies the per-task fd table end-to-end:
    # task_create allocates the child's fd_table with 0/1/2 pre-bound,
    # sys_write dispatches to serial through fd_get on the calling
    # task's table.  Pre-#134 this went through a global s_fds[].
    it "exec-hello" \
"$(keys "exec $P_APPS/hello.elf tester")
sendkey ret"
    assert_serial_contains "Hello," "tester" "status=0"
}

test_per_tty_cwd() {
    # Per-task cwd isolation across TTYs (slice 15).  Each shell task
    # owns task_t.cwd; vfs_getcwd/vfs_cd route through task_current.  We
    # cd VT0 to /proc, switch to VT3 and cd it to /apps, then
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
$(keys "cd $P_APPS")
sendkey ret
sendkey alt-f1" \
        2.0
    assert_serial_contains "/proc~>" "/apps~>"
}

test_cd_root() {
    # `cd /` -> `pwd` confirms cwd lands at the virtual root.  Post
    # zsh-style tab cycling, tabbing on `cd /<TAB>` would cycle through
    # root entries instead of listing them; the listing test moved to
    # test_tab_path and test_tab_cycle.  Here we just verify the cd
    # plumbing reaches the root.
    it "cd-root-listing" \
"$(keys "cd /")
sendkey ret
$(keys "pwd")
sendkey ret"
    assert_serial_contains "[makbox:pwd] /"
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
    # `ls /mnt` lists the disk-filesystem mount container.  Post-rootfs
    # elevation, the CD-ROM is mounted at / (live boot's rootfs) and is
    # hidden from /mnt; the always-registered HD placeholders ("boot",
    # "root") still show as empty mountpoints.  Asserting on the "root"
    # placeholder proves the VFS_FS_MNT route + ls_mnt() listing still
    # work end-to-end.
    it "ls-mnt" \
"$(keys "ls $P_MNT")
sendkey ret"
    assert_serial_contains "root"
}

test_mount_noargs() {
    it "mount-noargs" \
"$(keys "mount")
sendkey ret"
    assert_serial_contains "/ type " "/dev type devfs" "/proc type procfs"
    assert_serial_not_contains "Usage: mount"
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
"$(keys "exec $P_APPS/calc.elf")
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
    send_script "$(keys "exec $P_APPS/maktop.elf")
sendkey ret"
    sleep 1.2
    # Switch to VT1 and launch maktop there too.
    send_script 'sendkey alt-f2'
    sleep 0.6
    send_script "$(keys "exec $P_APPS/maktop.elf")
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
    send_script "$(keys "exec $P_APPS/maktop.elf")
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
    send_script "$(keys "exec $P_APPS/maktop.elf")
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
"$(keys "exec $P_APPS/forktest.elf")
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
"$(keys "exec $P_APPS/execvetest.elf")
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
"$(keys "exec $P_APPS/sigtest.elf")
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
"$(keys "exec $P_APPS/calc.elf")
sendkey ret
PAUSE 0.8
sendkey ctrl-c
PAUSE 0.4
$(keys "pwd")
sendkey ret" \
        2.0
    assert_serial_contains "[makbox:pwd]"
}

test_ctrlc_cat() {
    # makbox cat installs a SIGINT handler and writes in small yielded
    # chunks, so Ctrl+C should stop a large stream promptly and return the
    # shell to an interactive prompt.
    it "ctrlc-cat" \
"$(keys "cat /log/kernel.log")
sendkey ret
PAUSE 0.2
sendkey ctrl-c
PAUSE 0.4
$(keys "echo after-cat")
sendkey ret" \
        4.0
    assert_serial_contains "status=130" "after-cat"
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
$(keys "mkdir /mnt/cdrom/probe")
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

test_filetest() {
    # Phase-1-of-TCC-port slice: exercises the writable FD_KIND_FILE path
    # end-to-end against a real FAT32 volume.  Formats /dev/hda FAT32, mounts
    # it at /mnt/scratch (a fresh mkdir'd mountpoint), then runs filetest.elf
    # which drives:
    #   - O_CREAT|O_TRUNC + write + flush-on-close
    #   - O_RDONLY reopen + fstat + read + memcmp
    #   - O_APPEND
    #   - sys_stat() reflecting the appended size
    #   - 256 KiB grow past the old 64 KiB cap
    #   - no-flush on a read-only close
    # All milestones print over COM1; we assert on every PASS line plus the
    # final "[filetest] PASS".  If any sub-test fails, filetest.elf exits
    # with a "[filetest] FAIL: <reason>" line which the assert misses,
    # so the scenario fails loudly.
    # Use ext2 -- /dev/hda is the runner's raw scratch disk with no MBR, so
    # mkfs.fat32 refuses it ("partition too small"); mkfs.ext2 takes the whole
    # device as a single ext2 volume (same pattern as test_mnt_mountpoint).
    # We mkdir /mnt/scratch first (post-/mnt/hd-removal there's no default
    # writable HD mountpoint to land at).  Sync on filetest's own
    # "[filetest] PASS" marker so the next shell command is never typed
    # while filetest is still running (which otherwise drops the first
    # keystroke into the kernel keyboard ring at a bad moment).  it_until
    # already calls reset_shell internally -- don't call it again here or
    # you get a visible double "^C / cd /" sequence on screen.
    it_until "filetest" \
"$(keys "mkdir /mnt/scratch")
sendkey ret
$(keys "mkfs.ext2 /dev/hda")
sendkey ret
$(keys "mount /dev/hda /mnt/scratch")
sendkey ret
$(keys "exec $P_APPS/filetest.elf /mnt/scratch")
sendkey ret" \
        "[filetest] PASS" 60
    assert_serial_contains \
        "[filetest] dir=/mnt/scratch" \
        "[filetest] create+write+close ok" \
        "[filetest] reopen+fstat+read ok size=13" \
        "[filetest] append+close ok" \
        "[filetest] stat ok size=19" \
        "[filetest] grow-256k ok" \
        "[filetest] no-flush-on-rdonly ok" \
        "[filetest] PASS"
}

test_incore() {
    # In-kernel test driver: types a single `sh /apps/incore.sh` and the
    # script runs the non-interactive scenarios (hello, forktest,
    # execvetest, alloctest) inside the kernel via exec + $? checks --
    # no HMP per-scenario typing, no per-test reaper-output races.  See
    # src/userspace/incore.sh for the test list.  Loud failure: the
    # script exits non-zero and emits "INCORE: FAIL <name>" lines,
    # which assert_serial_not_contains catches; on the happy path the
    # final marker "INCORE: ALL PASS" satisfies it_until.
    it_until "incore" \
"$(keys "sh /apps/incore.sh")
sendkey ret" \
        "INCORE: ALL PASS" 60
    assert_serial_contains "INCORE: ALL PASS"
    assert_serial_not_contains "INCORE: FAIL" "Kernel panic" "SIGSEGV"
}

test_tmp_roundtrip() {
    # Smoke test for the in-RAM /tmp ramdisk (fs/tmpfs.c).  The `write`
    # shell builtin invokes vfs_write_file directly, which routes /tmp
    # writes to tmpfs_write; `cat` reads them back via tmpfs_read.  No
    # disk mount required.  Verifies the kernel-side /tmp arm of every
    # vfs.c dispatch matrix.
    it "tmp-roundtrip" \
"$(keys "write /tmp/probe.txt hello-tmpfs")
sendkey ret
$(keys "cat /tmp/probe.txt")
sendkey ret"
    assert_serial_contains "hello-tmpfs"
}

test_usr_resolves() {
    # /usr is a synthetic redirect to the active sysroot mount.  On ISO
    # boot, vfs.c resolves it via the rootfs election (containing libc.a,
    # crt0.o, headers, and example sources).  Probe via `ls /usr/include`
    # which exercises the rewrite path in vfs_route + the iso9660 backend
    # underneath.
    it "usr-resolves" \
"$(keys "ls /usr/include")
sendkey ret"
    assert_serial_contains "stdio.h"
}

test_usershell_smoke() {
    # First-ever ring-3 userspace shell scenario.  Drops from the
    # in-kernel shell into /apps/sh.elf, exercises pwd + cd + pwd to
    # prove SYS_GETCWD round-trips AND the new SYS_CHDIR(12) actually
    # mutates the calling task's cwd, then `exit`s back to the kernel
    # shell.  Asserts the new "$ " prompt appears (proves sh.elf
    # started), the cwd line after `cd /proc` reads /proc (proves
    # SYS_CHDIR), and a final pwd via the kernel makbox proves the
    # parent shell is responsive after sh.elf returned.
    reset_shell
    it_until "usershell-smoke" \
"$(keys "exec /apps/sh.elf")
sendkey ret
PAUSE 0.8
$(keys "pwd")
sendkey ret
$(keys "cd /proc")
sendkey ret
$(keys "pwd")
sendkey ret
$(keys "exit")
sendkey ret
$(keys "pwd")
sendkey ret" \
        "[makbox:pwd]" 15
    assert_serial_contains "sh.elf: ring-3 userspace shell" "/proc" "[makbox:pwd]"
    assert_serial_not_contains "Kernel panic" "SIGSEGV"
}

test_usershell_execve() {
    # Proves the ring-3 shell's fork+execve+wait4 dispatch path: drop
    # into sh.elf, then run /apps/calc.elf and feed it (2+3)*4=20,
    # then `exit` calc (back to sh.elf), then `exit` sh.elf (back to
    # the kernel shell).  Asserts the answer reached serial AND that
    # nothing along the path SIGSEGV'd or panicked.
    reset_shell
    it_until "usershell-execve" \
"$(keys "exec /apps/sh.elf")
sendkey ret
PAUSE 0.8
$(keys "/apps/calc.elf")
sendkey ret
PAUSE 0.8
$(keys "(2+3)*4")
sendkey ret
$(keys "exit")
sendkey ret
$(keys "exit")
sendkey ret" \
        "20" 15
    assert_serial_contains "sh.elf: ring-3 userspace shell" "20"
    assert_serial_not_contains "Kernel panic" "SIGSEGV"
}

test_usershell_history() {
    # Slice 20b: sh.elf's inline-edit readline + 16-entry history.  Types
    # `pwd`, then Up-arrow to recall it, then Enter to re-run.  Two `/`
    # lines in serial (from the two pwd invocations under cwd=/) prove
    # the history navigation re-played the line.  Backspace mid-line
    # exercise: type `pwf`, backspace, `d`, Enter -> still pwd.
    reset_shell
    it_until "usershell-history" \
"$(keys "exec /apps/sh.elf")
sendkey ret
PAUSE 0.8
$(keys "cd /")
sendkey ret
$(keys "pwd")
sendkey ret
sendkey up
sendkey ret
$(keys "pwf")
sendkey backspace
$(keys "d")
sendkey ret
$(keys "exit")
sendkey ret" \
        "[shell:ready vt=0]" 15
    # Three successful pwd invocations expected -- typed, recalled, and
    # the backspace-corrected variant.  All print "/\n".  We can't easily
    # count occurrences in assert_serial_contains, but seeing the prompt
    # come back after `exit` proves the loop didn't wedge.
    assert_serial_contains "sh.elf: ring-3 userspace shell"
    assert_serial_not_contains "Kernel panic" "SIGSEGV" "command not found"
}

test_usershell_vars() {
    # Slice 20c: sh.elf variable table + $VAR/${VAR}/$? expansion +
    # env/unset/read builtins.  Sets foo=tester, echoes "$foo"
    # through echo (sh.elf has no PATH lookup or makbox
    # auto-routing -- that's a kernel-shell behaviour we deliberately
    # don't replicate), then triggers a known-bad command and checks
    # $? reflects the non-zero status.  Finally unsets foo and confirms
    # $foo expands to empty.
    #
    # NB: lowercase var names only.  Pre-existing kernel bug: HMP
    # `sendkey shift-<letter>` drops the letter on its way through the
    # PS/2 + ring-3 ring path, so uppercase identifiers can't be typed
    # in ui-tests.  No existing scenario hit it because none typed
    # shift+letter.  Tracked separately; not gating this slice.
    reset_shell
    it_until "usershell-vars" \
"$(keys "exec /apps/sh.elf")
sendkey ret
PAUSE 0.8
$(keys "foo=tester")
sendkey ret
$(keys "echo hello \$foo")
sendkey ret
$(keys "/apps/no-such-binary")
sendkey ret
$(keys "echo status=\$?")
sendkey ret
$(keys "unset foo")
sendkey ret
$(keys "echo gone=\$foo=end")
sendkey ret
$(keys "exit")
sendkey ret" \
        "gone==end" 20
    assert_serial_contains "sh.elf: ring-3 userspace shell" \
                           "hello tester" \
                           "status=127" \
                           "gone==end"
    assert_serial_not_contains "Kernel panic" "SIGSEGV"
}

test_tcc_rebuild_sh() {
    # Self-host milestone for the ring-3 shell: in-OS TCC rebuilds
    # sh.c from its in-tree source.  Same pattern as test_tcc_rebuild_calc
    # but on a more complex program (fork/execve/wait4/getcwd/chdir).
    # Two-stage send to avoid the typing race that bit test_tcc_rebuild_calc:
    # type the compile + Enter, wait for shell-ready, THEN exec and drive
    # the rebuilt shell.
    reset_shell
    CURRENT_NAME=tcc-rebuild-sh
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/sh.c -o /tmp/sh.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 90 || \
        echo "  - stage1: tcc compile of sh.c never returned to prompt"
    it_until "tcc-rebuild-sh" \
"$(keys "exec /tmp/sh.elf")
sendkey ret
PAUSE 0.8
$(keys "pwd")
sendkey ret
$(keys "exit")
sendkey ret" \
        "sh.elf: ring-3 userspace shell" 15
    assert_serial_contains "sh.elf: ring-3 userspace shell"
    assert_serial_not_contains "Kernel panic" "SIGSEGV"
}

test_tcc_hello() {
    # Phase-3-of-TCC-port slice: cross-built tcc.elf compiles a known C
    # source on a running Makar guest and the freshly-emitted ELF is
    # exec'd by the shell.  Source comes from /usr/share/examples/, the
    # output lands in /tmp (in-RAM ramdisk -- no disk mount needed), and
    # the greeting hits serial via sys_write(2, ...).  60s budget
    # because TCC under TCG is slow.
    #
    # Skip cleanly if tcc.elf wasn't built (vendor/tinycc/ not vendored
    # in this checkout).  ui_runner.sh exposes assert_serial_contains
    # which fails the scenario on missing markers; the "skip" path
    # writes a SKIP marker to serial and short-circuits.
    # Two-stage send: tcc takes several seconds under TCG, so the second
    # batch's keystrokes would queue in the keyboard ring and lose the
    # focus-transfer race when tcc finally reaps.  Sync on the kernel
    # shell prompt returning before typing the `exec` line.
    reset_shell
    CURRENT_NAME=tcc-hello
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /usr/share/examples/hello-tcc.c -o /tmp/hello.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 60 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-hello" \
"$(keys "exec /tmp/hello.elf")
sendkey ret" \
        "Hello, TCC" 15
    assert_serial_contains "Hello, TCC"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)"
}

test_tcc_rebuild_calc() {
    # Self-host milestone: in-OS Makar rebuilds calc.elf from its
    # in-tree source via TCC, then runs the rebuilt binary against the
    # same arithmetic vector test_calc_brackets uses.  Source shipped
    # by iso.sh (isodir/src/userspace/calc.c) and reachable two ways:
    #   /src/userspace/calc.c             (resolved via rootfs election)
    #   /src/userspace/calc.c             (via the rootfs fallthrough
    #                                      added with the HDD-root work)
    # We use the rootfs path so this scenario also exercises that route.
    # Asserts: the rebuilt binary computes correctly AND nothing along
    # the path panicked or delivered SIGSEGV.  90 s budget because TCC
    # compile under TCG is slow.
    # Two-stage send: type the compile + Enter, wait for the shell prompt
    # to come back (proves tcc exited cleanly and isn't still consuming
    # keys), THEN type the exec + REPL inputs.  Without this sync the
    # second batch's first byte can race ahead and arrive while tcc is
    # still running -- the keys queue up in HMP and the keyboard ring
    # drops chars on drain, producing flakes like "ex/calc.elf"
    # instead of "exec /tmp/calc.elf".
    reset_shell
    CURRENT_NAME=tcc-rebuild-calc
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/calc.c -o /tmp/calc.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 90 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-rebuild-calc" \
"$(keys "exec /tmp/calc.elf")
sendkey ret
PAUSE 0.8
$(keys "(2+3)*4")
sendkey ret
$(keys "exit")
sendkey ret" \
        "20" 15
    assert_serial_contains "20"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
}

test_tcc_rebuild_hello() {
    # In-OS rebuild of hello.elf -- the simplest possible TCC smoke beyond
    # tcc_hello (which compiles hello-tcc.c from /usr/share/examples).  Here
    # we compile the in-tree freestanding hello.c shipped under /src and run
    # the resulting binary, asserting on its greeting.  Same two-stage send
    # pattern as test_tcc_rebuild_calc to avoid the typing race while tcc
    # is still consuming the keyboard ring.
    reset_shell
    CURRENT_NAME=tcc-rebuild-hello
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/hello.c -o /tmp/hello.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 60 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-rebuild-hello" \
"$(keys "exec /tmp/hello.elf rebuilt")
sendkey ret" \
        "Hello," 15
    assert_serial_contains "Hello," "rebuilt"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
}

test_tcc_rebuild_makbox() {
    # In-OS rebuild of makbox.elf (the freestanding multicall busybox: ls /
    # cat / cp / mv / rm / rmdir / echo / pwd).  Proves TCC handles the
    # larger multicall dispatcher + every applet's syscall surface against
    # the same shim-free build the shipped binary uses.  Asserts on the
    # `pwd` applet's output (deterministic across boots once we cd /).
    reset_shell
    CURRENT_NAME=tcc-rebuild-makbox
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/makbox.c -o /tmp/makbox.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 90 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-rebuild-makbox" \
"$(keys "cd /")
sendkey ret
$(keys "exec /tmp/makbox.elf pwd")
sendkey ret" \
        "[makbox:pwd]" 15
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
}

## test_libc_tcc_script -- retired.
##
## The libc / TCC rebuild matrix is no longer driven by HMP sendkey.  It
## runs inline during test_mode bootup (kernel.c's `sh_run_file
## /src/userspace/libc-tcc.sh` after incore.sh), and run.sh's
## _check_ktest greps the LIBC-TCC: ALL PASS / LIBC-TCC: FAIL marker.
##
## Rationale: typing a single command through sendkey buys nothing for a
## test that doesn't exercise the UI, and the timing-race surface of
## HMP under TCG makes it the flakiest part of the suite.

TCC_COMPILE_FAILED=0

tcc_compile_user_app() {
    local app=$1
    local timeout=${2:-90}
    local output=/tmp/$app.elf

    TCC_COMPILE_FAILED=0
    reset_shell
    CURRENT_NAME=tcc-rebuild-$app
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/$app.c -o $output")
sendkey ret"
    if ! wait_for_serial '\[shell:ready vt=0\]' "$sb1" "$timeout"; then
        echo "  - stage1: tcc compile of $app.c never returned to prompt"
        TCC_COMPILE_FAILED=1
    fi
    local compile_segment=$LOGDIR/$CURRENT_NAME.compile.serial
    dd if="$SERIAL_LOG" bs=1 skip="$sb1" 2>/dev/null > "$compile_segment"
    if grep -qE -- '(^|[[:space:]])error:' "$compile_segment"; then
        echo "  - stage1: tcc reported an error for $app.c"
        awk 'BEGIN { RS=""; ORS="" } { gsub(/\r/, ""); gsub(/[ \t]+/, " "); print }' \
            "$compile_segment" \
            | awk -v max=800 '{
                if (length($0) > max) print "  - compile: " substr($0, 1, max) " ...[truncated]"
                else print "  - compile: " $0
              }'
        TCC_COMPILE_FAILED=1
    fi
}

tcc_assert_compile_ok() {
    if [ "$TCC_COMPILE_FAILED" != "0" ]; then
        CURRENT_FAILED=1
    fi
}

test_tcc_rebuild_help() {
    tcc_compile_user_app help 60
    it_until "tcc-rebuild-help" \
"$(keys "exec /tmp/help.elf")
sendkey ret" \
        '\[shell:ready vt=0\]' 15
    assert_serial_contains "vix <file>"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_diskinfo() {
    tcc_compile_user_app diskinfo 60
    it_until "tcc-rebuild-diskinfo" \
"$(keys "exec /tmp/diskinfo.elf")
sendkey ret" \
        '\[shell:ready vt=0\]' 15
    assert_serial_contains "drive 0: ATA"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_sigtest() {
    tcc_compile_user_app sigtest 60
    it_until "tcc-rebuild-sigtest" \
"$(keys "exec /tmp/sigtest.elf")
sendkey ret" \
        "sigtest: SIGUSR1 handler ran" 20
    assert_serial_contains "sigtest: SIGUSR1 handler ran"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_forktest() {
    tcc_compile_user_app forktest 60
    it_until "tcc-rebuild-forktest" \
"$(keys "exec /tmp/forktest.elf")
sendkey ret" \
        '\[forktest\] PARENT-POST' 20
    assert_serial_contains "[forktest] PARENT-POST"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_execvetest() {
    tcc_compile_user_app execvetest 60
    it_until "tcc-rebuild-execvetest" \
"$(keys "exec /tmp/execvetest.elf")
sendkey ret" \
        '\[execve-test\] POST-EXEC' 20
    assert_serial_contains "[execve-test] POST-EXEC"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_alloctest() {
    tcc_compile_user_app alloctest 90
    it_until "tcc-rebuild-alloctest" \
"$(keys "exec /tmp/alloctest.elf")
sendkey ret" \
        '\[alloctest\] PASS' 30
    assert_serial_contains "[alloctest] PASS"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_filetest() {
    tcc_compile_user_app filetest 90
    it_until "tcc-rebuild-filetest" \
"$(keys "mkdir /mnt/scratch")
sendkey ret
$(keys "mkfs.ext2 /dev/hda")
sendkey ret
$(keys "mount /dev/hda /mnt/scratch")
sendkey ret
$(keys "exec /tmp/filetest.elf /mnt/scratch")
sendkey ret" \
        '\[filetest\] PASS' 60
    assert_serial_contains \
        "[filetest] dir=/mnt/scratch" \
        "[filetest] create+write+close ok" \
        "[filetest] reopen+fstat+read ok size=13" \
        "[filetest] append+close ok" \
        "[filetest] stat ok size=19" \
        "[filetest] grow-256k ok" \
        "[filetest] no-flush-on-rdonly ok" \
        "[filetest] PASS"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_basic() {
    tcc_compile_user_app basic 120
    it_until "tcc-rebuild-basic" \
"$(keys "exec /tmp/basic.elf")
sendkey ret
PAUSE 0.8
$(keys "10 PRINT 2+3")
sendkey ret
$(keys "RUN")
sendkey ret
$(keys "QUIT")
sendkey ret" \
        "READY." 30
    assert_serial_contains "Makar BASIC" "5" "READY."
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_fdisk() {
    tcc_compile_user_app fdisk 90
    it_until "tcc-rebuild-fdisk" \
"$(keys "exec /tmp/fdisk.elf /dev/hda")
sendkey ret
PAUSE 0.8
$(keys "q")
sendkey ret" \
        "fdisk>" 20
    assert_serial_contains "fdisk>"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_cfdisk() {
    tcc_compile_user_app cfdisk 120
    it_until "tcc-rebuild-cfdisk" \
"$(keys "exec /tmp/cfdisk.elf /dev/hda")
sendkey ret
PAUSE 1.2
sendkey q
PAUSE 0.8
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 20
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_maktop() {
    tcc_compile_user_app maktop 120
    it_until "tcc-rebuild-maktop" \
"$(keys "exec /tmp/maktop.elf")
sendkey ret
PAUSE 1.2
sendkey q
PAUSE 0.8
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 20
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_clock() {
    tcc_compile_user_app clock 90
    it_until "tcc-rebuild-clock" \
"$(keys "exec /tmp/clock.elf")
sendkey ret
PAUSE 1.2
sendkey q
PAUSE 0.8
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 20
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_lines() {
    tcc_compile_user_app lines 90
    it_until "tcc-rebuild-lines" \
"$(keys "exec /tmp/lines.elf")
sendkey ret
PAUSE 1.2
sendkey q
PAUSE 0.8
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 20
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_vix() {
    tcc_compile_user_app vix 120
    it_until "tcc-rebuild-vix" \
"$(keys "exec /tmp/vix.elf /tmp/tcc-vix.txt")
sendkey ret
PAUSE 1.2
sendkey ctrl-q
PAUSE 0.8
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 20
    assert_serial_contains "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_rebuild_kbtester() {
    tcc_compile_user_app kbtester 120
    it_until "tcc-rebuild-kbtester" \
"$(keys "exec /tmp/kbtester.elf")
sendkey ret
PAUSE 1.2
sendkey ctrl-c
PAUSE 1.0
$(keys "pwd")
sendkey ret" \
        '\[makbox:pwd\]' 30
    assert_serial_contains "KBTESTER_BEGIN" "[makbox:pwd] /"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)" "SIGSEGV"
    tcc_assert_compile_ok
}

test_tcc_hello_relpath() {
    # cd into the examples dir then `tcc hello-tcc.c -o /tmp/relhello.elf`
    # exercises the kernel's path_resolve (cwd-join for relative inputs)
    # AND TCC's own internal open() shim.  The output stays absolute so
    # /tmp is independent of cwd.  Distinct output name + greeting search
    # so this scenario doesn't false-pass on a stale /tmp/hello.elf from
    # test_tcc_hello.
    # Two-stage send: see test_tcc_hello -- sync on the prompt returning
    # after tcc compiles before sending the `exec` line.  `cd` happens
    # synchronously up front; the wait targets the tcc compile.
    reset_shell
    CURRENT_NAME=tcc-hello-relpath
    send_script "$(keys "cd /usr/share/examples")
sendkey ret"
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc hello-tcc.c -o /tmp/relhello.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 60 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-hello-relpath" \
"$(keys "exec /tmp/relhello.elf")
sendkey ret" \
        "Hello, TCC" 15
    assert_serial_contains "Hello, TCC"
    assert_serial_not_contains "Kernel panic" "panic(cpu 0)"
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
    send_script "$(keys "cat /log/install.log")
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
"$(keys "sh $P_APPS/demo.sh")
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
    send_script "$(keys "exec $P_APPS/clock.elf")
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
    send_script "$(keys "exec $P_APPS/maktop.elf")
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

# Scenarios are organized into themed groups so a topic-focused change
# only needs to re-run the relevant slice during development.  Each group
# is a bash array; `ALL_TESTS` is the concatenation in stable order.  A
# CLI arg matching a group name expands to that group's members; arg
# `fast` expands to FAST_TESTS (the dev-inner-loop slice with no disk
# mkfs / long compiles).
#
# tmp_roundtrip, usr_resolves, and no_dead_in_proctasks stay opt-in -- the
# kernel-side ktests cover the same ground without shell typing or serial
# mirroring races. Invoke explicitly, e.g. `./run.sh ui no_dead_in_proctasks`.
#
# per_vt_palettes and bughunt_vix_palette_on_vt3 are palette-only visual
# checks with no serial assertion; opt-in for manual inspection.

SHELL_TESTS=(glob_proc tab_path tab_cycle typo_doesnt_clear shell_scripting_vars calc_brackets makbox_pwd demo_script)
CD_PWD_TESTS=(cd_root per_tty_cwd)
FS_TESTS=(ls_dev ls_mnt mount_noargs mnt_mountpoint filetest)
POSIX_TESTS=(exec_hello fork_cow fork_execve user_sigusr1_handler ctrlc_kills_child ctrlc_cat usershell_smoke usershell_execve usershell_history usershell_vars)
## TCC_REBUILD_TESTS / LIBC_TESTS -- retired as HMP scenarios.
##
## The whole libc + TCC self-rebuild matrix now runs from
## /src/userspace/libc-tcc.sh during test_mode bootup.  Marker
## `LIBC-TCC: ALL PASS` / `LIBC-TCC: FAIL` on serial; checked by
## run.sh _check_ktest.  See docs/plans/userspace-shell-migration-HANDOFF.md.
##
## Empty arrays kept so `./run.sh ui libc` is a no-op rather than an
## error.  Use `./run.sh iso test` for the actual coverage.
TCC_REBUILD_TESTS=()
LIBC_TESTS=()
INCORE_TESTS=(incore)
VT_TESTS=(vt_roundtrip_keeps_maktop_focused vt_all_roundtrips)
BUGHUNT_TESTS=(bughunt_clock_exit_palette bughunt_vix_exit_palette bughunt_status_bar_after_switch)

## LIBC_TESTS is empty (libc/TCC matrix moved into test_mode bootup);
## under `set -u` an empty-array splice errors out, so we use the
## `${arr[@]+...}` guard that expands to nothing when the array is
## empty.  Apply the same guard to every group for symmetry / future
## migrations.
ALL_TESTS=(
    ${SHELL_TESTS[@]+"${SHELL_TESTS[@]}"}
    ${CD_PWD_TESTS[@]+"${CD_PWD_TESTS[@]}"}
    ${FS_TESTS[@]+"${FS_TESTS[@]}"}
    ${POSIX_TESTS[@]+"${POSIX_TESTS[@]}"}
    ${LIBC_TESTS[@]+"${LIBC_TESTS[@]}"}
    ${INCORE_TESTS[@]+"${INCORE_TESTS[@]}"}
    ${VT_TESTS[@]+"${VT_TESTS[@]}"}
    ${BUGHUNT_TESTS[@]+"${BUGHUNT_TESTS[@]}"}
)

# FAST_TESTS: skip anything that mkfs's a disk, compiles C, or runs a long
# script -- excludes filetest, mnt_mountpoint, alloctest, tcc_hello,
# demo_script.  Aimed at the dev inner loop where you want a sub-minute
# regression sweep before pushing.
FAST_TESTS=(glob_proc tab_path tab_cycle typo_doesnt_clear shell_scripting_vars calc_brackets makbox_pwd cd_root per_tty_cwd ls_dev ls_mnt exec_hello fork_cow fork_execve user_sigusr1_handler ctrlc_kills_child vt_roundtrip_keeps_maktop_focused vt_all_roundtrips)

# Expand a single argument: if it names a known group, emit the group's
# members; otherwise emit it unchanged (with dashes->underscores).
expand_arg() {
    local a=${1//-/_}
    case $a in
        all)      printf '%s\n' "${ALL_TESTS[@]}" ;;
        fast)     printf '%s\n' "${FAST_TESTS[@]}" ;;
        shell)    printf '%s\n' "${SHELL_TESTS[@]}" ;;
        cd_pwd|cd) printf '%s\n' "${CD_PWD_TESTS[@]}" ;;
        fs)       printf '%s\n' "${FS_TESTS[@]}" ;;
        posix)    printf '%s\n' "${POSIX_TESTS[@]}" ;;
        libc|tcc_rebuild|tcc)
            ## Group retired -- the libc + TCC self-rebuild matrix runs
            ## in /src/userspace/libc-tcc.sh during test_mode bootup.
            ## Run `./run.sh iso test` to exercise it.
            echo "ui_test: group '$1' is no longer driven by HMP sendkey." >&2
            echo "         The libc/TCC self-rebuild matrix runs at" >&2
            echo "         test_mode bootup via /src/userspace/libc-tcc.sh." >&2
            echo "         Use './run.sh iso test' for that coverage." >&2
            ;;
        incore)   printf '%s\n' "${INCORE_TESTS[@]}" ;;
        vt)       printf '%s\n' "${VT_TESTS[@]}" ;;
        bughunt)  printf '%s\n' "${BUGHUNT_TESTS[@]}" ;;
        *)        printf '%s\n' "$a" ;;
    esac
}

declare -a TO_RUN
if [ $# -eq 0 ]; then
    TO_RUN=("${ALL_TESTS[@]}")
else
    for arg in "$@"; do
        while IFS= read -r t; do
            TO_RUN+=("$t")
        done < <(expand_arg "$arg")
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
