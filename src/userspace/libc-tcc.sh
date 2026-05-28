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

# --- ar applet smoke (slice 5b: kernel-self-rebuild rung #2 -- can
# in-OS `ar` stitch object files into a static archive?).  Uses the
# /tmp/hello.o from compile-hello-c above plus a fresh one to give it
# something to archive.
echo LIBC-TCC: compile-hello-c-2
tcc -c /src/userspace/calc.c -o /tmp/calc.o
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-hello-c-2; else echo LIBC-TCC: [FAIL] compile-hello-c-2; fail=1; fi
sleep $pause
echo LIBC-TCC: ar-rcs
exec /apps/ar.elf rcs /tmp/test.a /tmp/hello.o /tmp/calc.o
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] ar-rcs; else echo LIBC-TCC: [FAIL] ar-rcs; fail=1; fi
sleep $pause

# --- tcc as the linker (slice 5c rung -- can we link in-OS-built .o
# files into a runnable ELF?).  TCC accepts mixed .c and .o inputs --
# this one's all-objects so it's pure link.  Pulls crt1/crti/crtn from
# /usr/lib so the result is a complete ring-3 executable.
echo LIBC-TCC: link-hello-from-o
tcc /tmp/hello.o -o /tmp/hello-from-o.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] link-hello-from-o; else echo LIBC-TCC: [FAIL] link-hello-from-o; fail=1; fi
sleep $pause
echo LIBC-TCC: run-hello-from-o
exec /tmp/hello-from-o.elf link-from-o
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] run-hello-from-o; else echo LIBC-TCC: [FAIL] run-hello-from-o; fail=1; fi
sleep $pause

# --- Freestanding link (slice 5c rung-2: tcc -nostdlib produces a kernel-
# shaped ELF).  Validates that the in-OS TCC's linker driver works without
# crt0/libc -- the prerequisite for actually linking a Makar kernel binary.
echo LIBC-TCC: compile-freestanding
tcc -c -ffreestanding -nostdlib /src/userspace/freestanding.c -o /tmp/free.o
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] compile-freestanding; else echo LIBC-TCC: [FAIL] compile-freestanding; fail=1; fi
sleep $pause

echo LIBC-TCC: link-freestanding
tcc -nostdlib -static /tmp/free.o -o /tmp/free.bin
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] link-freestanding; else echo LIBC-TCC: [FAIL] link-freestanding; fail=1; fi
sleep $pause

# --- TCC's -T flag isn't supported in v0.9.27 (tcc: error: invalid option).
# Try driving the same layout via -Wl, flags as a workaround.  Just
# setting the text load address gets us most of the way to a kernel-
# shaped binary -- a real Multiboot 2 header still needs to be emitted
# by the C source (or a tiny .S file) but that's a separate piece.
echo LIBC-TCC: link-freestanding-wl-ttext
tcc -nostdlib -static -Wl,-Ttext,0x100000 /tmp/free.o -o /tmp/free-1m.bin
if [ $? -eq 0 ]; then echo LIBC-TCC: [PASS] link-freestanding-wl-ttext; else echo LIBC-TCC: [FAIL] link-freestanding-wl-ttext; fail=1; fi
sleep $pause

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

# --- TCC self-rebuild (v1.0 leg-1): currently skipped in CI for TWO
# reasons documented in docs/plans/v1.md:
#   1. The compile exceeds the iso-test watchdog (mitigated by the
#      KTEST_TIMEOUT=360 default that run.sh now sets -- override
#      KTEST_TIMEOUT to extend further).
#   2. After TCC exits in this run, the kernel sh interpreter enters
#      a state where subsequent script lines silently no-op
#      (subsequent SHELL-SMOKE / sh_run_file calls return without
#      processing).  This blocks running tcc-self in the same boot
#      as shell-smoke.sh.  Suspect: TCC's stdout fd or the
#      kernel-shell wait4 path leaves stale state behind.
#
# Manual invocation (boot interactively into a shell, then):
#   tcc -DONE_SOURCE=1 -DCONFIG_TCC_STATIC \
#       -DCONFIG_TCCDIR="/usr/lib/tcc" \
#       -DCONFIG_TCC_SYSINCLUDEPATHS="/usr/include:/usr/lib/tcc/include" \
#       -I/src/tinycc -I/src/tinycc/build-stubs -I/src/userspace \
#       /src/tinycc/tcc.c -o /tmp/tcc-rebuilt.elf
#
# v1.0 leg 1 ships once issue #2 is rooted out and this can re-enable.
