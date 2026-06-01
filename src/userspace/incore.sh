#!/bin/sh
# incore.sh -- in-kernel UI test driver.
#
# Runs the subset of UI scenarios that don't depend on interactive
# kernel features (TAB, Ctrl-C, VT switching, sh.elf readline,
# fullscreen apps).  Each test runs an ELF whose own exit status (0 =
# pass, non-zero = fail) is checked via $? -- no serial substring
# matching required.  Final marker is "INCORE: ALL PASS" (or "INCORE:
# FAIL"); the runner greps for that.
#
# Trade-off vs HMP-driven ui_test.sh: faster (no QEMU sendkey pacing /
# settle), no typing races, but loses screendump evidence on panic.
# Use HMP scenarios for anything that needs to type or read the
# framebuffer; use incore.sh for "run a binary, check it exited clean"
# style coverage.
#
# NB: the kernel sh tokenizer doesn't yet strip double quotes (see
# shell.c:shell_parse) -- avoid quotes; use bare words throughout.

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

echo INCORE: RUN alloctest
exec /apps/alloctest.elf
if [ $? -eq 0 ]; then echo INCORE: PASS alloctest; else echo INCORE: FAIL alloctest; fail=1; fi

echo INCORE: RUN ktest_uspace
exec /apps/ktest_uspace.elf
if [ $? -eq 0 ]; then echo INCORE: PASS ktest_uspace; else echo INCORE: FAIL ktest_uspace; fail=1; fi

if [ $fail -eq 0 ]; then echo INCORE: ALL PASS; else echo INCORE: FAIL; fi
