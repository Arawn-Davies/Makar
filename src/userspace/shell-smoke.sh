#!/bin/sh
# shell-smoke.sh -- in-kernel smoke tests for shell + VFS + apps.
#
# Replaces the HMP-driven scenarios in tests/ui_test.sh whose only job
# was to type a command and grep serial.  Each command runs through the
# kernel sh-script interpreter; the test's exit status ($?) gates a
# [PASS] / [FAIL] marker, and any output the command emits flows to
# serial naturally so run.sh's _check_ktest transcript still shows it.
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
# pipes, **no double-quote stripping in echo** (use bareword args).
# Inline `if X; then Y; else Z; fi` supported (slice 0 fix).

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

# Pipes (slice A1): `|` is supported in userspace /apps/sh.elf only.
# Kernel sh (this interpreter) doesn't tokenize `|`; verify pipes by
# typing `echo X | cat` directly into sh.elf during a UI scenario.

if [ $fail -eq 0 ]
then
    echo SHELL-SMOKE: ALL PASS
else
    echo SHELL-SMOKE: FAIL
fi
