#!/bin/sh
# libc-tcc.sh -- in-kernel TCC/libc regression driver.
#
# This script is intentionally run from /src/userspace, not installed under
# /apps.  It keeps the HMP/ui runner from typing a long compile matrix while
# still exercising in-OS TCC, the shipped sysroot, and libc.a.
#
# Kernel sh limitations: no functions, no command substitution, no pipes.
# Keep everything line-oriented and avoid quotes.

echo LIBC-TCC: BEGIN
fail=0

echo LIBC-TCC: COMPILE hello
tcc /src/userspace/hello.c -o /tmp/hello.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-hello; else echo LIBC-TCC: FAIL compile-hello; fail=1; fi
exec /tmp/hello.elf libc
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-hello; else echo LIBC-TCC: FAIL run-hello; fail=1; fi

echo LIBC-TCC: COMPILE calc
tcc /src/userspace/calc.c -o /tmp/calc.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-calc; else echo LIBC-TCC: FAIL compile-calc; fail=1; fi

echo LIBC-TCC: COMPILE sh
tcc /src/userspace/sh.c -o /tmp/sh.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-sh; else echo LIBC-TCC: FAIL compile-sh; fail=1; fi

echo LIBC-TCC: COMPILE makbox
tcc /src/userspace/makbox.c -o /tmp/makbox.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-makbox; else echo LIBC-TCC: FAIL compile-makbox; fail=1; fi
exec /tmp/makbox.elf pwd
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-makbox; else echo LIBC-TCC: FAIL run-makbox; fail=1; fi

echo LIBC-TCC: COMPILE help
tcc /src/userspace/help.c -o /tmp/help.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-help; else echo LIBC-TCC: FAIL compile-help; fail=1; fi
exec /tmp/help.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-help; else echo LIBC-TCC: FAIL run-help; fail=1; fi

echo LIBC-TCC: COMPILE diskinfo
tcc /src/userspace/diskinfo.c -o /tmp/diskinfo.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-diskinfo; else echo LIBC-TCC: FAIL compile-diskinfo; fail=1; fi
exec /tmp/diskinfo.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-diskinfo; else echo LIBC-TCC: FAIL run-diskinfo; fail=1; fi

echo LIBC-TCC: COMPILE sigtest
tcc /src/userspace/sigtest.c -o /tmp/sigtest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-sigtest; else echo LIBC-TCC: FAIL compile-sigtest; fail=1; fi
exec /tmp/sigtest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-sigtest; else echo LIBC-TCC: FAIL run-sigtest; fail=1; fi

echo LIBC-TCC: COMPILE forktest
tcc /src/userspace/forktest.c -o /tmp/forktest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-forktest; else echo LIBC-TCC: FAIL compile-forktest; fail=1; fi
exec /tmp/forktest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-forktest; else echo LIBC-TCC: FAIL run-forktest; fail=1; fi

echo LIBC-TCC: COMPILE execvetest
tcc /src/userspace/execvetest.c -o /tmp/execvetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-execvetest; else echo LIBC-TCC: FAIL compile-execvetest; fail=1; fi
exec /tmp/execvetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-execvetest; else echo LIBC-TCC: FAIL run-execvetest; fail=1; fi

echo LIBC-TCC: COMPILE alloctest
tcc /src/userspace/alloctest.c -o /tmp/alloctest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-alloctest; else echo LIBC-TCC: FAIL compile-alloctest; fail=1; fi
exec /tmp/alloctest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-alloctest; else echo LIBC-TCC: FAIL run-alloctest; fail=1; fi

echo LIBC-TCC: COMPILE filetest
tcc /src/userspace/filetest.c -o /tmp/filetest.elf
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS compile-filetest; else echo LIBC-TCC: FAIL compile-filetest; fail=1; fi
exec /tmp/filetest.elf /tmp
if [ $? -eq 0 ]; then echo LIBC-TCC: PASS run-filetest; else echo LIBC-TCC: FAIL run-filetest; fail=1; fi

if [ $fail -eq 0 ]; then echo LIBC-TCC: ALL PASS; else echo LIBC-TCC: FAIL; fi
