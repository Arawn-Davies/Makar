#!/bin/sh
# shell-smoke.sh -- in-kernel smoke tests for shell + VFS + apps.
#
# In-guest shell/VFS/apps coverage: each command runs through the kernel
# sh-script interpreter (or /apps/sh.elf -c for ring-3 shell behaviour) and
# its exit status ($?) gates a [PASS] / [FAIL] marker; any output flows to
# serial naturally so run.sh's _check_ktest transcript still shows it.  No
# host input -- keyboard-under-test paths live in keyboard_test_driver().
#
# Marker contract:
#   SHELL-SMOKE: BEGIN
#   SHELL-SMOKE: <name>           -- printed before each test runs
#   SHELL-SMOKE: [PASS] <name>    -- on success
#   SHELL-SMOKE: [FAIL] <name>    -- on failure
#   SHELL-SMOKE: ALL PASS         -- iff every test was PASS
#   SHELL-SMOKE: FAIL             -- otherwise
#
# Kernel sh limitations: no functions, no command substitution, no
# pipes.  Quote stripping IS supported now (shell_parse is quote-aware:
# '...'/"..." group a word and the quotes are removed).  Inline
# `if X; then Y; else Z; fi` supported (slice 0 fix).

echo SHELL-SMOKE: BEGIN
fail=0

echo SHELL-SMOKE: ls-proc
ls /proc
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] ls-proc
else
    echo SHELL-SMOKE: [FAIL] ls-proc
    fail=1
fi

echo SHELL-SMOKE: exec-hello
exec /apps/hello.elf tester
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] exec-hello
else
    echo SHELL-SMOKE: [FAIL] exec-hello
    fail=1
fi

# Static-musl smoke (dynamic-linking epic, Phase 0): proves a *real* musl libc
# ELF runs in-OS -- musl startup (auxv walk, set_thread_area TLS, futex locks)
# and printf->write(1)->exit all work.  Same flat top-level `exec` shape as
# exec-hello above (the kernel sh's `exec` doesn't survive an if/else nest).
# The binary prints "hello from musl libc" to serial; clean exit (0) -> PASS.
# Always staged by toolchain/build-musl-demos.sh before `iso build`/`iso test`.
echo SHELL-SMOKE: musl-static
exec /apps/muslhello.elf musltest
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] musl-static
else
    echo SHELL-SMOKE: [FAIL] musl-static
    fail=1
fi

# Dynamic-musl smoke (Phase 3/4): a PIE linked against musl's shared libc.so.
# The kernel loads PT_INTERP=/lib/ld-musl-i386.so.1, which mmaps /lib/libc.so,
# relocates, and jumps to the program -- the whole dynamic-linking runtime.
echo SHELL-SMOKE: musl-dynamic
exec /apps/muslhellodyn.elf musldyntest
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] musl-dynamic
else
    echo SHELL-SMOKE: [FAIL] musl-dynamic
    fail=1
fi

echo SHELL-SMOKE: cd-root
cd /
pwd
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] cd-root
else
    echo SHELL-SMOKE: [FAIL] cd-root
    fail=1
fi

echo SHELL-SMOKE: ls-dev
ls /dev
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] ls-dev
else
    echo SHELL-SMOKE: [FAIL] ls-dev
    fail=1
fi

# /dev/ttyN: the VT slots are addressable as Linux-style terminal nodes.
# tty3 is a background slot here (no makmux running under the smoke driver),
# so writing to it just lands in that VT's backing grid -- a clean exit (0)
# proves open+write routed through devfs DEV_TTY -> vtty_write.
echo SHELL-SMOKE: dev-tty-write
echo vtwrite > /dev/tty3
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] dev-tty-write
else
    echo SHELL-SMOKE: [FAIL] dev-tty-write
    fail=1
fi

echo SHELL-SMOKE: ls-mnt
ls /mnt
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] ls-mnt
else
    echo SHELL-SMOKE: [FAIL] ls-mnt
    fail=1
fi

echo SHELL-SMOKE: mount-noargs
mount
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] mount-noargs
else
    echo SHELL-SMOKE: [FAIL] mount-noargs
    fail=1
fi

echo SHELL-SMOKE: makbox-pwd
pwd
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] makbox-pwd
else
    echo SHELL-SMOKE: [FAIL] makbox-pwd
    fail=1
fi

echo SHELL-SMOKE: scripting-vars
name=foo
env
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] scripting-vars
else
    echo SHELL-SMOKE: [FAIL] scripting-vars
    fail=1
fi

echo SHELL-SMOKE: tmp-roundtrip
write /tmp/probe.txt hello-tmpfs
cat /tmp/probe.txt
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] tmp-roundtrip
else
    echo SHELL-SMOKE: [FAIL] tmp-roundtrip
    fail=1
fi

echo SHELL-SMOKE: usr-resolves
ls /usr/include
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] usr-resolves
else
    echo SHELL-SMOKE: [FAIL] usr-resolves
    fail=1
fi

echo SHELL-SMOKE: proc-tasks
cat /proc/tasks
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] proc-tasks
else
    echo SHELL-SMOKE: [FAIL] proc-tasks
    fail=1
fi

echo SHELL-SMOKE: nested-sh-parser-state
sh /src/userspace/nested-probe.sh
if [ 1 -eq 1 ]
then
    echo SHELL-SMOKE: [PASS] nested-sh-parser-state
else
    echo SHELL-SMOKE: [FAIL] nested-sh-parser-state
    fail=1
fi

echo SHELL-SMOKE: inline-if-else-then
result=neither
if [ 1 -eq 1 ]; then result=ran-then; else result=ran-else; fi
if [ $result = ran-then ]
then
    echo SHELL-SMOKE: [PASS] inline-if-else-then
else
    echo SHELL-SMOKE: [FAIL] inline-if-else-then
    fail=1
fi

echo SHELL-SMOKE: inline-if-else-else
result=neither
if [ 0 -eq 1 ]; then result=ran-then; else result=ran-else; fi
if [ $result = ran-else ]
then
    echo SHELL-SMOKE: [PASS] inline-if-else-else
else
    echo SHELL-SMOKE: [FAIL] inline-if-else-else
    fail=1
fi

# Quote-aware tokeniser: a quoted multi-word string must collapse to ONE
# argument so the `[ A = B ]` test sees exactly 3 inner tokens and matches.
# If quotes were kept literal (the old bug), the inner arg count would be
# wrong and the test would fail.
# NB: glob expansion is an interactive-shell (shell.c) feature, not part of
# the kernel sh-script interpreter, so it's exercised via the live prompt
# (keyboard_test_driver), not here.

echo SHELL-SMOKE: tty-name
tty
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] tty-name
else
    echo SHELL-SMOKE: [FAIL] tty-name
    fail=1
fi

echo SHELL-SMOKE: quote-grouping
if [ "a b" = "a b" ]
then
    echo SHELL-SMOKE: [PASS] quote-grouping
else
    echo SHELL-SMOKE: [FAIL] quote-grouping
    fail=1
fi

# --- /apps/sh.elf (ring-3) control flow + quoting, slice 20d ---
# Driven headlessly through `sh.elf -c '<payload>'`; the payload's exit
# status proves which branch ran.  Payloads carry no `$` so this kernel
# sh's pre-expansion can't touch them (the single quotes are stripped by
# shell_parse, the inner text reaches sh.elf verbatim).  Covers the ring-3
# control-flow + quoting paths headlessly (no host input).

echo SHELL-SMOKE: usersh-if-then
# then-branch runs `false` -> exit 1 (if the else-branch had run it'd be 0).
exec /apps/sh.elf -c 'if [ 1 -eq 1 ]; then false; else true; fi'
rc=$?
if [ $rc -eq 1 ]
then
    echo SHELL-SMOKE: [PASS] usersh-if-then
else
    echo SHELL-SMOKE: [FAIL] usersh-if-then
    fail=1
fi

echo SHELL-SMOKE: usersh-if-else
# else-branch runs `true` -> exit 0.
exec /apps/sh.elf -c 'if [ 1 -eq 2 ]; then false; else true; fi'
rc=$?
if [ $rc -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] usersh-if-else
else
    echo SHELL-SMOKE: [FAIL] usersh-if-else
    fail=1
fi

echo SHELL-SMOKE: usersh-for-loop
# loop body runs `false` once -> exit 1 (proves the body executed).
exec /apps/sh.elf -c 'for i in one; do false; done'
rc=$?
if [ $rc -eq 1 ]
then
    echo SHELL-SMOKE: [PASS] usersh-for-loop
else
    echo SHELL-SMOKE: [FAIL] usersh-for-loop
    fail=1
fi

echo SHELL-SMOKE: usersh-quote
# quotes grouped+removed -> `[ a b = a b ]` (3 inner args) -> true (0).
exec /apps/sh.elf -c '[ "a b" = "a b" ]'
rc=$?
if [ $rc -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] usersh-quote
else
    echo SHELL-SMOKE: [FAIL] usersh-quote
    fail=1
fi

# Pipes (slice A1): `|` is supported in userspace /apps/sh.elf only.
# Kernel sh (this interpreter) doesn't tokenize `|`; verify pipes by
# typing `echo X | cat` directly into sh.elf during a UI scenario.

# GUI widget framework self-test (no framebuffer touched: uitest returns
# before sys_fb_info).  Also emits its own GUI-UITEST: PASS marker.
echo SHELL-SMOKE: gui-uitest
exec /apps/gui.elf uitest
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] gui-uitest
else
    echo SHELL-SMOKE: [FAIL] gui-uitest
    fail=1
fi

# GUI file-browser navigation self-test against the real VFS (chdir/readdir/
# getcwd) -- proves the Files window + editor dialog actually move about the
# filesystem.  Emits its own GUI-FSTEST: PASS marker.
echo SHELL-SMOKE: gui-fstest
exec /apps/gui.elf fstest
if [ $? -eq 0 ]
then
    echo SHELL-SMOKE: [PASS] gui-fstest
else
    echo SHELL-SMOKE: [FAIL] gui-fstest
    fail=1
fi

if [ $fail -eq 0 ]
then
    echo SHELL-SMOKE: ALL PASS
else
    echo SHELL-SMOKE: FAIL
fi
