#!/bin/sh
# incore.sh -- in-guest "run a binary, check it exited clean" test driver.
#
# Headless: each test runs an ELF whose own exit status (0 = pass,
# non-zero = fail) is checked via $? -- no keystrokes, no serial
# substring matching.  Final marker "INCORE: ALL PASS" (or "INCORE:
# FAIL"); the runner greps for that.  Paths where the keystroke itself
# is under test live in keyboard_test_driver() (./run.sh kbtest); other
# shell behaviour lives in shell-smoke.sh.

echo INCORE: BEGIN
fail=0

echo INCORE: RUN hello
exec /apps/hello.elf incore
if [ $? -eq 0 ]; then echo INCORE: PASS hello; else echo INCORE: FAIL hello; fail=1; fi

echo INCORE: RUN forktest
exec /apps/forktest.elf
if [ $? -eq 0 ]; then echo INCORE: PASS forktest; else echo INCORE: FAIL forktest; fail=1; fi

echo INCORE: RUN execvetest
exec /apps/execvetest.elf
if [ $? -eq 0 ]; then echo INCORE: PASS execvetest; else echo INCORE: FAIL execvetest; fail=1; fi

echo INCORE: RUN sigtest
exec /apps/sigtest.elf
if [ $? -eq 0 ]; then echo INCORE: PASS sigtest; else echo INCORE: FAIL sigtest; fail=1; fi

echo INCORE: RUN alloctest
exec /apps/alloctest.elf
if [ $? -eq 0 ]; then echo INCORE: PASS alloctest; else echo INCORE: FAIL alloctest; fail=1; fi

echo INCORE: RUN ktest_uspace
exec /apps/ktest_uspace.elf
if [ $? -eq 0 ]; then echo INCORE: PASS ktest_uspace; else echo INCORE: FAIL ktest_uspace; fail=1; fi

if [ $fail -eq 0 ]; then echo INCORE: ALL PASS; else echo INCORE: FAIL; fi
