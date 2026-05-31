# userspace (Ring 3)

Freestanding ring-3 ELF binaries for Makar. Built with the i686-elf
cross-compiler against `crt0.S` + `libc.a`; loaded and executed by the
kernel's ELF loader via the `exec` shell command or `SYS_EXECVE`.

All programs use the `int 0x80` Linux i386 syscall ABI — see
`syscall.h` (inline stubs) and `docs/syscalls.md` for the full table.

## Structure

```
src/userspace/
  sh.c            # POSIX-shaped login shell (mak.sh0 / mak.sh1-4)
  makmux.c        # VT multiplexer — status bar + 4 child sh.elf instances
  makbox.c        # Busybox multicall: ls, cat, cp, mv, rm, rmdir, echo, pwd
  vix.c           # vi-style text editor
  tcc.c           # TinyCC v0.9.27 in-OS C compiler
  basic.c         # C64-flavoured integer BASIC
  calc.c          # bc-style expression calculator
  clock.c         # Fullscreen RTC wall-clock
  maktop.c        # Task monitor (ps/top style)
  fdisk.c         # MBR partition editor (line-driven)
  cfdisk.c        # Full-screen cfdisk-style MBR editor
  kbtester.c      # Keyboard diagnostic — logs events to serial
  diskinfo.c      # Partition table + FAT32 BPB dump
  hello.c         # Hello-world smoke test
  forktest.c      # fork/exec/wait smoke test
  execvetest.c    # execve smoke test
  alloctest.c     # malloc/libc smoke tests (12 sub-tests)
  filetest.c      # VFS file I/O smoke test
  sigtest.c       # Signal delivery smoke test
  lines.c         # Animated line-drawing demo (BASIC companion)
  malloc.c        # Heap allocator
  stdio.c         # printf / fopen / fread / fwrite / fclose
  string.c        # string.h implementation
  crt0.S          # C runtime startup (_start → main → SYS_EXIT)
  link.ld         # Linker script (USER_CODE_BASE = 0x40000000)
  syscall.h       # Inline int 0x80 syscall stubs
  demo.sh         # Shell script example (ships as /apps/demo.sh)
  incore.sh       # In-kernel UI test driver
  *.bas           # BASIC sample programs (mandelbrot.bas, lines.bas)
```

## Building

Built automatically by `build.sh` (via `./run.sh iso build`). The
cross-compiler target is `i686-elf`; no host libc is used.

## Syscall ABI

`int 0x80`, EAX = syscall number, args in EBX/ECX/EDX/ESI/EDI, return in EAX.
Follows the Linux i386 convention for the standard calls (1–265).
Makar extensions start at 200. Full reference: `docs/syscalls.md`.
