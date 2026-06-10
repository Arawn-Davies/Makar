---
title: POSIX compliance
parent: Reference
nav_order: 2
---

# POSIX Compliance

Makar is not a UNIX system and does not claim SUS/POSIX conformance. It does,
however, intentionally use a POSIX-shaped userspace model: Linux i386 syscall
numbers where practical, static ELF programs, file descriptors, fork/exec/wait,
signals, a small libc, and a shell that can run real scripts.

This page is written for application porters. It answers: what can a small C
program rely on today, what is only a compatibility stub, and what still needs
kernel work?

## Current Headline

Makar currently supports:

- ring-3 ELF programs loaded by `elf_exec`
- `fork`, `execve`, `wait4`, `getpid`, `getppid`
- per-task file descriptor tables
- `open`, `read`, `write`, `close`, `lseek`, `stat`, `fstat`
- `dup`, `dup2`, `pipe`
- `brk`, userspace `malloc`, and anonymous `mmap`
- basic signal delivery and user signal handlers
- UTC time APIs
- x87/SSE FPU state saved/restored per task
- one-slot i386 TLS through `set_thread_area`
- dynamically-linked PIEs against a shared musl `libc.so`, loaded by musl's own
  interpreter `ld-musl-i386.so.1` (`ET_DYN` + `PT_INTERP` + full auxv)
- a userspace shell with pipes, redirection, list operators, background jobs,
  `wait`, quoting, and `sh -c`

It does not currently support:

- users, groups, permissions, or credentials
- process groups, sessions, or job-control terminal ownership
- pthreads or clone-style user threads
- networking or sockets
- a supported in-place JIT (`tcc -run`-style) execution model — TCC compiles to
  files and `exec`s them instead
- complete Linux signal-mask semantics
- `select`, `poll`, or another fd readiness API
- a termios-compatible terminal interface

## Process Model

| Feature | Status | Notes |
|---|---|---|
| `fork` | Present | Copy-on-write page-directory clone via `task_fork`. |
| `execve` | Present | Replaces the current image with an ELF. `envp` is accepted but ignored. |
| `wait4` | Present | Supports `WNOHANG`; `rusage` is ignored. |
| `exit` | Present | Terminates the current task. |
| `exit_group` | Stub | Same as `exit`; Makar has one userspace thread per process. |
| `getpid`, `getppid` | Present | Expose `task_t.pid` and `task_t.parent_pid`. |
| `set_tid_address` | Stub | Returns the task pid for hosted libc startup. |
| `vfork`, `posix_spawn` | Absent | Native COW fork exists, so these are not currently needed. |
| `clone`, pthreads | Absent | No userspace thread groups. |

PIDs are monotonic for the boot session. The idle task is pid 1. Task slots are
reused internally after a task becomes `TASK_DEAD`, but pid values keep
advancing.

## File Descriptors and VFS

| Feature | Status | Notes |
|---|---|---|
| fd table | Present | Each task has its own fd table. |
| stdin/stdout/stderr | Present | fd 0 = keyboard/stdin, fd 1 = terminal/stdout, fd 2 = terminal + serial/stderr. |
| `open` | Present | Supports common read/write/create/truncate/append flags; `mode` is ignored. |
| `read`, `write` | Present | File, pipe, terminal, and keyboard paths. |
| `close` | Present | Flushes dirty file buffers. |
| `lseek` | Present | `SEEK_SET`, `SEEK_CUR`, `SEEK_END`. |
| `dup` | Present | Allocates the lowest free fd. |
| `dup2` | Present | Duplicates onto a requested fd, closing it first. |
| `pipe` | Present | 4 KiB ring with reader/writer refcounts. |
| `fcntl` | Partial | `F_GETFL` and `F_SETFL`; only documented bits are meaningful. |
| `stat`, `fstat` | Partial | Linux-shaped `struct stat`, limited fields, no permission enforcement. |
| `readdir` | Present | Makar-indexed syscall backing libc `DIR *` wrappers. |

The VFS supports ISO9660, FAT32, ext2, tmpfs, devfs, procfs, and logfs. Not
every filesystem supports every mutation. `/tmp` is the reliable scratch
location during live boots.

## Memory APIs

| Feature | Status | Notes |
|---|---|---|
| `brk` | Present | Main heap-growth mechanism for the userspace malloc shim. |
| `mmap2` | Anonymous + file-backed (RO shared) | Anon non-fixed = demand-paged bump window at `0x90000000`. A **read-only** file map shares frames from the page cache (`pagecache_acquire`), so libc.so's text/rodata is one copy in RAM across all mappers; writable/tmpfs file maps and anon-fixed take a private frame. Backs musl `ld.so`'s library maps. |
| `munmap` | Present | Unmaps the range and releases each frame (`vmm_unmap_and_free`, refcount-decrement — shared cache frames survive while others map them). Address reuse within the bump window is not implemented. |
| `MAP_FIXED` | Present | Maps at the caller's exact address, replacing any existing mapping in that window. |
| `mprotect` | Present | `SYS_MPROTECT` (125) rewrites PTE R/W/USER over the range (`ld.so` RELRO). i386 non-PAE has no NX, so X is implicit. |
| executable mmap/JIT | Unsupported | TCC compiles to files and `exec`s them; `tcc -run` is not the supported model. |

Anonymous `mmap` exists because hosted libc allocators, including musl paths,
expect it for larger allocations. The implementation is intentionally simple:
zero-filled frames are mapped into a bump region and are not reused during the
process lifetime.

## Signals

| Feature | Status | Notes |
|---|---|---|
| `kill` | Present | Sends a signal to a pid. |
| `signal` | Present | Installs a simple handler or disposition. |
| user handlers | Present | Delivered through a userspace sigframe and `sigreturn`. |
| default termination | Present | Fatal default signals mark the task dead. |
| `sigreturn` | Present | Internal trampoline syscall. |
| `rt_sigprocmask` | Stub | Returns success for hosted libc startup; real masking is not complete. |
| `sigaction` | Absent | Use `signal` for now. |
| `sigsuspend`, `pause` | Absent | No blocking signal-wait API. |
| process groups | Absent | No job-control signal model. |

Signal support is good enough for current apps and tests (`sigtest.elf`) but
not yet a complete POSIX signal subsystem.

## Time

| Feature | Status | Notes |
|---|---|---|
| `gettimeofday` | Present | Seconds from CMOS RTC; microseconds are PIT-derived at 10 ms resolution. |
| `clock_gettime` | Present | `CLOCK_REALTIME` and `CLOCK_MONOTONIC`. |
| `time` | Present | Userspace libc wrapper. |
| `gmtime`, `gmtime_r` | Present | Integer-only UTC conversion. |
| `localtime`, `localtime_r` | Present | Alias UTC; no timezone database. |
| `mktime` | Present | UTC interpretation. |
| `strftime` | Partial | Common numeric/date specifiers. |
| `nanosleep` | Absent | `sleep`/`usleep` spin-yield on the 250 Hz tick. |

There is no locale or timezone database. Treat all broken-down time as UTC.

## Floating Point

The kernel initializes the x87 FPU and enables FXSAVE/FXRSTOR where available.
Each task has a 512-byte FPU state area, and the scheduler saves/restores it
around context switches. Floating-point state is therefore multitask-safe.

What is still missing is the userspace libc surface: there is no shipped
`<math.h>` implementation and no complete floating-point formatting/parsing
library.

## TLS and Hosted libc Startup

Both static and dynamically-linked i386 musl startup need a small
Linux-compatible substrate. Makar now provides:

- a full System-V i386 ELF auxv: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`,
  `AT_ENTRY`, `AT_EXECFN`, `AT_RANDOM`, `AT_PAGESZ`, `AT_NULL`
- `set_thread_area(243)` using one GDT TLS slot at selector `0x33`
- `%gs` preservation across interrupts
- scheduler TLS restore for TLS-active tasks
- `writev` (musl's buffered stdio writes through it)
- `exit_group`
- `set_tid_address`
- `rt_sigprocmask` stub
- `ioctl` returning `-ENOTTY`
- `futex` stub for the current single-threaded bring-up assumption

For **dynamic** linking the loader additionally honours `PT_INTERP`
(`/lib/ld-musl-i386.so.1`, == `libc.so`), enters at the interpreter, and the
kernel services musl's `ld.so` runtime: file-backed/`MAP_FIXED` `mmap2`,
`mprotect` (RELRO), and `open` returning `-ENOENT` so the library search walks.

These are compatibility pieces, not full Linux implementations. They exist to
get hosted binaries through early libc initialization and dynamic relocation.

## Userspace libc Headers

| Header | Current coverage | Important gaps |
|---|---|---|
| `<stdio.h>` | `FILE *` I/O, `printf` family subset, fd-backed reads/writes | no wide char, no `popen`, incomplete buffering controls |
| `<stdlib.h>` | allocation, conversions, `qsort`, `bsearch`, env table, `system`, `sscanf` | env does not cross `execve`; no locale/multibyte family |
| `<string.h>` | memory/string basics, `strtok`, `strtok_r`, `strerror` | no collation/locale transforms |
| `<unistd.h>` | common declarations and thin wrappers: fd I/O, `dup`, `pipe`, pids, cwd, sleep | no `alarm`, `pause`; no POSIX `fork`/`exec` wrappers yet |
| `<dirent.h>` | `opendir`, `readdir`, `closedir` | backed by indexed syscall rather than Linux `getdents` |
| `<errno.h>` | common errno constants and global `errno` | not every syscall wrapper sets errno yet |
| `<time.h>` | UTC broken-down time and `strftime` subset | no timezone, no `ctime`/`asctime`/`difftime` |
| `<setjmp.h>` | i386 `setjmp`/`longjmp` | no signal-mask variants |
| `<ctype.h>` | ASCII C-locale classifiers | no locale |
| `<math.h>` | absent | FPU substrate exists, library does not |
| `<pthread.h>` | absent | no user threading |

## Shell Compatibility

The default interactive shell is `/apps/sh.elf`. It supports:

- variables and `$?`
- quoting with quote removal
- `sh -c`
- `if`, `while`, `for`
- `[ ... ]`
- pipelines
- redirection
- `&&`, `||`
- background `&`
- `wait`

The in-kernel script interpreter is smaller. It is used for boot/test scripts
and intentionally does not support pipes, redirection, list operators, or
background jobs. Use `/apps/sh.elf` for scripts that need those features.

Known shell deviations:

- no command substitution
- no shell functions
- no `case`
- no subshell syntax
- no true job control
- `$` expansion currently happens before tokenization, so single quotes do not
  fully suppress variable expansion

## Terminal Model

Makar does not have a POSIX `termios` layer. Programs use Makar-specific
syscalls for terminal size, cursor position, drawing, raw keyboard mode, and
caret style. Virtual terminals and app tabs are managed through Makar VT
extensions and `makmux`.

The `tty` command prints Linux-shaped names:

- `/dev/console` for the root console
- `/dev/ttyN` for VT slots
- `not a tty` for unbound tasks

## App-Porter Checklist

When porting a small C program:

1. Prefer static builds.
2. Avoid threads.
3. Avoid file-backed mmap and `mprotect`.
4. Use `/tmp` for scratch writes during live boots.
5. Expect all local time to be UTC.
6. Avoid termios; use Makar terminal syscalls or simple stdio.
7. Prefer `/apps/sh.elf` if the program shells out through `system`.
8. Check both return values and `errno`; errno coverage is improving but not
   universal.
9. Run `alloctest.elf`, `libc-tcc.sh`, and `shell-smoke.sh` when adding libc
   surface.
10. Run `kbtest` when changing keyboard, VT, makmux, shell focus, or app-tab
    behavior.
