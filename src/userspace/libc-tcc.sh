#!/bin/sh
# libc-tcc.sh -- in-kernel TCC/libc regression driver.
#
# This script is intentionally run from /src/userspace, not installed under
# /apps.  It keeps the HMP/ui runner from typing a long compile matrix while
# still exercising in-OS TCC, the shipped sysroot, and libc.a.
#
# Marker contract:
#   LIBC-TCC: BEGIN
#   LIBC-TCC: <name>              -- printed before each test runs
#   LIBC-TCC: [PASS] <name>       -- on success
#   LIBC-TCC: [FAIL] <name>       -- on failure
#   LIBC-TCC: ALL PASS            -- iff every test was PASS
#   LIBC-TCC: FAIL                -- otherwise
#
# Kernel sh limitations: no functions, no command substitution, no pipes.
# Keep everything line-oriented and avoid quotes.

echo LIBC-TCC: BEGIN
fail=0
pause=0.5

echo LIBC-TCC: compile-hello
tcc /src/userspace/hello.c -o /tmp/hello.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-hello; else echo LIBC-TCC: [FAIL] compile-hello; fail=1; fi
sleep $pause
echo LIBC-TCC: run-hello
exec /tmp/hello.elf libc
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-hello; else echo LIBC-TCC: [FAIL] run-hello; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-calc
tcc /src/userspace/calc.c -o /tmp/calc.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-calc; else echo LIBC-TCC: [FAIL] compile-calc; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-sh
tcc /src/userspace/sh.c -o /tmp/sh.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-sh; else echo LIBC-TCC: [FAIL] compile-sh; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-makbox
tcc /src/userspace/makbox.c -o /tmp/makbox.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-makbox; else echo LIBC-TCC: [FAIL] compile-makbox; fail=1; fi
sleep $pause
echo LIBC-TCC: run-makbox
exec /tmp/makbox.elf pwd
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-makbox; else echo LIBC-TCC: [FAIL] run-makbox; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-makmux
tcc /src/userspace/makmux.c -o /tmp/makmux.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-makmux; else echo LIBC-TCC: [FAIL] compile-makmux; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-help
tcc /src/userspace/help.c -o /tmp/help.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-help; else echo LIBC-TCC: [FAIL] compile-help; fail=1; fi
sleep $pause
echo LIBC-TCC: run-help
exec /tmp/help.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-help; else echo LIBC-TCC: [FAIL] run-help; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-diskinfo
tcc /src/userspace/diskinfo.c -o /tmp/diskinfo.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-diskinfo; else echo LIBC-TCC: [FAIL] compile-diskinfo; fail=1; fi
sleep $pause
echo LIBC-TCC: run-diskinfo
exec /tmp/diskinfo.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-diskinfo; else echo LIBC-TCC: [FAIL] run-diskinfo; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-sigtest
tcc /src/userspace/sigtest.c -o /tmp/sigtest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-sigtest; else echo LIBC-TCC: [FAIL] compile-sigtest; fail=1; fi
sleep $pause
echo LIBC-TCC: run-sigtest
exec /tmp/sigtest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-sigtest; else echo LIBC-TCC: [FAIL] run-sigtest; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-forktest
tcc /src/userspace/forktest.c -o /tmp/forktest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-forktest; else echo LIBC-TCC: [FAIL] compile-forktest; fail=1; fi
sleep $pause
echo LIBC-TCC: run-forktest
exec /tmp/forktest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-forktest; else echo LIBC-TCC: [FAIL] run-forktest; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-execvetest
tcc /src/userspace/execvetest.c -o /tmp/execvetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-execvetest; else echo LIBC-TCC: [FAIL] compile-execvetest; fail=1; fi
sleep $pause
echo LIBC-TCC: run-execvetest
exec /tmp/execvetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-execvetest; else echo LIBC-TCC: [FAIL] run-execvetest; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-alloctest
tcc /src/userspace/alloctest.c -o /tmp/alloctest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-alloctest; else echo LIBC-TCC: [FAIL] compile-alloctest; fail=1; fi
sleep $pause
echo LIBC-TCC: run-alloctest
exec /tmp/alloctest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-alloctest; else echo LIBC-TCC: [FAIL] run-alloctest; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-filetest
tcc /src/userspace/filetest.c -o /tmp/filetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-filetest; else echo LIBC-TCC: [FAIL] compile-filetest; fail=1; fi
sleep $pause
echo LIBC-TCC: run-filetest
exec /tmp/filetest.elf /tmp
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-filetest; else echo LIBC-TCC: [FAIL] run-filetest; fail=1; fi
sleep $pause

# --- Compile-only checks for the interactive / fullscreen apps -------------
# These exist primarily to prove TCC + libc.a link the app correctly.  The
# apps themselves are interactive (basic REPL, fullscreen editors) so we
# skip the run step -- regressions there are caught by manual UI scenarios.

echo LIBC-TCC: compile-basic
tcc /src/userspace/basic.c -o /tmp/basic.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-basic; else echo LIBC-TCC: [FAIL] compile-basic; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-fdisk
tcc /src/userspace/fdisk.c -o /tmp/fdisk.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-fdisk; else echo LIBC-TCC: [FAIL] compile-fdisk; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-cfdisk
tcc /src/userspace/cfdisk.c -o /tmp/cfdisk.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-cfdisk; else echo LIBC-TCC: [FAIL] compile-cfdisk; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-maktop
tcc /src/userspace/maktop.c -o /tmp/maktop.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-maktop; else echo LIBC-TCC: [FAIL] compile-maktop; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-clock
tcc /src/userspace/clock.c -o /tmp/clock.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-clock; else echo LIBC-TCC: [FAIL] compile-clock; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-lines
tcc /src/userspace/lines.c -o /tmp/lines.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-lines; else echo LIBC-TCC: [FAIL] compile-lines; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-vix
tcc /src/userspace/vix.c -o /tmp/vix.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-vix; else echo LIBC-TCC: [FAIL] compile-vix; fail=1; fi
sleep $pause

echo LIBC-TCC: compile-kbtester
tcc /src/userspace/kbtester.c -o /tmp/kbtester.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-kbtester; else echo LIBC-TCC: [FAIL] compile-kbtester; fail=1; fi
sleep $pause

# --- TCC `-c` smoke: verify compile-only mode emits a valid object.
# Doubles as slice 5a evidence (kernel-self-rebuild rung #1 -- "can in-OS
# TCC produce a .o?").  Lightweight, doesn't time out.
echo LIBC-TCC: compile-hello-c
tcc -c /src/userspace/hello.c -o /tmp/hello.o
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-hello-c; else echo LIBC-TCC: [FAIL] compile-hello-c; fail=1; fi
sleep $pause

# --- TCC self-rebuild: the v1.0 self-host flex.  SKIPPED in the default
# iso-test run because compiling tcc.c on TCG-emulated i386 exceeds the
# QEMU watchdog (~3 min).  Enable manually with TCC_SELF=1:
#
#   tcc -DONE_SOURCE=1 -DCONFIG_TCC_STATIC \
#       -DCONFIG_TCCDIR="/usr/lib/tcc" \
#       -DCONFIG_TCC_SYSINCLUDEPATHS="/usr/include:/usr/lib/tcc/include" \
#       -I/src/tinycc -I/src/tinycc/build-stubs -I/src/userspace \
#       /src/tinycc/tcc.c -o /tmp/tcc-rebuilt.elf
#
# Known issues in current state (see docs/plans/v1.md for the full thread):
#   - vsnprintf %l/%ll/%z/%h modifiers (FIXED in this session).
#   - vsnprintf %s now guards against unmapped pointers via brk-aware
#     range check (FIXED in this session).
#   - TCC's source-location pointer is uninitialised on entry, so
#     diagnostics print "(badptr):543517801" instead of file:line.
#     Cosmetic; doesn't block compilation.
#   - Watchdog timeout: the actual compile would likely succeed if the
#     iso-test runner gave it more wall-clock budget.

# --- Relative-path TCC: prove path resolution works when cwd != /
cd /src/userspace
echo LIBC-TCC: compile-hello-relpath
tcc hello.c -o /tmp/hello-relpath.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-hello-relpath; else echo LIBC-TCC: [FAIL] compile-hello-relpath; fail=1; fi
sleep $pause
echo LIBC-TCC: run-hello-relpath
exec /tmp/hello-relpath.elf libc-relpath
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-hello-relpath; else echo LIBC-TCC: [FAIL] run-hello-relpath; fail=1; fi
sleep $pause
cd /

if [ $fail -eq 0 ]; then echo LIBC-TCC: ALL PASS; else echo LIBC-TCC: FAIL; fi
sleep 2
