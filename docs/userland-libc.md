---
title: Userland libc
parent: Reference
nav_order: 3
---

# Userland libc

Makar has two C-library layers:

| Layer | Path | Built into | Purpose |
|---|---|---|---|
| kernel freestanding libc | `src/libc/` | `libk.a` | tiny support library for the kernel itself |
| userspace libc shim | `src/userspace/` | `/usr/lib/libc.a` | hosted-ish static libc for ring-3 apps and TinyCC |

This page describes the userspace libc shim and the ongoing path toward static
musl binaries.

## Design Goals

The userspace libc is not trying to become glibc. Its goals are narrower:

1. Provide enough C/POSIX surface for Makar's own apps.
2. Provide enough hosted behavior for `tcc.elf` to compile and link programs
   inside the OS.
3. Keep every function small enough to audit.
4. Match Linux/POSIX shapes where useful so future musl/uClibc work is not
   boxed in by Makar-only APIs.
5. Remain statically linked and simple.

The long-term compatibility target is static musl i386. The practical in-OS
compiler remains TinyCC.

## Installed Sysroot

`src/userspace/Makefile` builds the userspace shim and stages it into the OS
image:

| Installed path | Contents |
|---|---|
| `/usr/lib/crt0.o` | userspace process entry stub |
| `/usr/lib/libc.a` | static userspace libc shim |
| `/usr/include/*.h` | userspace C headers |
| `/usr/share/examples/hello-tcc.c` | canonical in-OS compile example |

The same makefile also stages apps under `/apps`.

Generated dependency files (`*.d`) are enabled with `-MMD -MP` so changes to
headers such as `syscall.h` rebuild the apps that include them.

## Source Layout

| File | Role |
|---|---|
| `crt0.S` | initial process entry, argc/argv setup, call `main`, then exit |
| `malloc.{h,c}` | `malloc`, `free`, `calloc`, `realloc` over `SYS_BRK` |
| `stdio.{h,c}` | `FILE *` implementation and printf-family support |
| `stdlib.h` | declarations/inlines for allocation, conversion, env, `system`, sort/search |
| `string.{h,c}` | memory/string functions, tokenizer helpers, `strerror` |
| `setjmp.{h,S}` | i386 SysV `setjmp`/`longjmp` |
| `dirent.h` | POSIX-shaped directory iteration wrappers |
| `time.{h,c}` | UTC broken-down time conversion |
| `unistd.h` | POSIX-shaped declarations and thin syscall wrappers |
| `tcc_compat.c` | hosted wrapper functions used heavily by TinyCC |
| `syscall.h` | inline `int 0x80` wrappers and syscall constants |

## Current Header Surface

### `<stdio.h>`

Implemented:

- `FILE`
- `stdin`, `stdout`, `stderr` style behavior through fd wrappers where needed
- `fopen`
- `fdopen`
- `fread`
- `fwrite`
- `fclose`
- `fflush`
- `fputs`
- `fputc`
- `fgetc`
- `printf`
- `fprintf`
- `sprintf`
- `snprintf`
- `vsnprintf`
- `vfprintf`

Not complete:

- no wide-character I/O
- no `popen`
- no complete POSIX buffering controls
- formatting is intentionally small and integer-focused

### `<stdlib.h>`

Implemented:

- `malloc`
- `free`
- `calloc`
- `realloc`
- `atoi`
- `strtol`
- `strtoll`
- `strtod`/`strtof` placeholders where needed by TCC paths
- `qsort`
- `bsearch`
- `getenv`
- `setenv`
- `unsetenv`
- `putenv`
- `system`
- `exit`
- `abort`
- `sscanf`

Environment variables are process-local. They do not cross `execve`, because
the kernel currently ignores the `envp` pointer passed to `SYS_EXECVE`.

`system(command)` forks and runs:

```sh
/apps/sh.elf -c "$command"
```

It returns the child exit status low byte, or `-1` if the shell could not be
started.

### `<string.h>`

Implemented:

- `memcmp`
- `memcpy`
- `memmove`
- `memset`
- `strlen`
- `strcpy`
- `strncpy`
- `strcat`
- `strcmp`
- `strncmp`
- `strchr`
- `strrchr`
- `strstr`
- `strdup`
- `strtok`
- `strtok_r`
- `strerror`

The implementation assumes the C locale.

### `<unistd.h>`

Implemented or declared:

- `read`
- `write`
- `close`
- `lseek`
- `dup`
- `dup2`
- `pipe`
- `chdir`
- `getcwd`
- `getpid`
- `getppid`
- `unlink`
- `rmdir`
- `access`
- `sleep`
- `usleep`
- `_exit`

`sleep` and `usleep` spin-yield on the 100 Hz uptime tick. There is no
`nanosleep` syscall and no `SIGALRM` interruption behavior.

### `<time.h>`

Implemented:

- `time`
- `gmtime`
- `gmtime_r`
- `localtime`
- `localtime_r`
- `mktime`
- `strftime` subset

All local time is UTC. There is no timezone database.

### Other Headers

| Header | Status |
|---|---|
| `<ctype.h>` | ASCII classifiers and case conversion |
| `<dirent.h>` | `DIR *` wrappers over Makar indexed readdir |
| `<errno.h>` | common errno definitions and global `errno` |
| `<setjmp.h>` | i386 `setjmp`/`longjmp` |
| `<fcntl.h>` | common open flags |
| `<sys/stat.h>` | limited `struct stat` and constants |
| `<signal.h>` | signal constants and wrappers |

Missing major families:

- `<pthread.h>`
- `<locale.h>`
- `<wchar.h>`
- `<math.h>`

The kernel FPU substrate exists, but libc math functions do not.

## Kernel ABI Dependencies

The userspace libc shim depends on these kernel surfaces:

| Kernel feature | Status |
|---|---|
| Linux i386 `int 0x80` convention | present |
| fd tables | present |
| file read/write fds | present |
| `brk` | present |
| anonymous `mmap2` / `munmap` | present |
| `fork` / `execve` / `wait4` | present |
| `pipe` / `dup` / `dup2` | present |
| `stat` / `fstat` / `readdir` | present |
| signals and sigreturn | present, partial POSIX |
| FPU save/restore | present |
| TLS through `set_thread_area` | present |
| auxv | present |

## Static musl Bring-Up

The branch adds the main pieces a static i386 musl binary expects during early
startup:

- ELF auxv:
  - `AT_PAGESZ`
  - `AT_RANDOM`
  - `AT_NULL`
- `set_thread_area(243)`:
  - one TLS GDT slot at index 6
  - `%gs = 0x33`
  - scheduler restore for TLS-active tasks
- `exit_group(252)` as process exit
- `set_tid_address(258)` returning the task pid
- `rt_sigprocmask(175)` startup stub
- `ioctl(54)` returning `-ENOTTY`
- `futex(240)` startup stub for the single-threaded bring-up case
- anonymous `mmap2(192)`
- `munmap(91)`
- FPU state preservation

Remaining work for a comfortable musl environment:

- implement any missing startup syscall discovered by real binaries
- implement `writev` if the chosen musl build path requires it
- make errno behavior consistent across wrappers
- package musl headers and archives into the Makar sysroot
- add more complete signal-mask semantics
- decide how much thread support is actually desired
- support file-backed mmap and `mprotect` if future programs need them

The immediate proof target is a tiny static musl i386 program linked high
enough for Makar's loader, such as:

```c
int main(void) { return 42; }
```

## TinyCC vs musl

TinyCC currently uses the Makar libc shim. This is deliberate:

- the shim is small enough to debug inside the OS
- TCC's needs are known and covered by `libc-tcc.sh`
- the generated binaries are static ET_EXEC files that Makar can load directly

musl is the next compatibility target for outside programs. It does not
replace the value of the small in-tree shim for OS-native development.

## Test Coverage

Use these tests when changing libc or syscall behavior:

```sh
./run.sh ktest
./run.sh gui libc
./run.sh gui all-tests
```

Important in-guest checks:

- `alloctest.elf`: allocation, stdlib, env, tokenizer, `bsearch`, pipe/dup,
  time, `system`, anonymous mmap
- `libc-tcc.sh`: compiles and runs the in-OS TCC/libc matrix
- `shell-smoke.sh`: exercises `sh -c`, quoting, and command behavior

Run `kbtest` as well if the change touches syscall numbers, shell focus,
keyboard routing, VT behavior, or makmux.
