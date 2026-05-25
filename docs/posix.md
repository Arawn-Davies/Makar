# POSIX compliance

Snapshot of where Makar sits relative to POSIX.1-2017 / SUSv4.  Not
comprehensive -- focused on what an app port needs to know.  See
`hello.c`'s opening comment for the official "Makar is not, in fact,
a UNIX®" disclaimer.

## Headline

Makar implements a **POSIX-shaped subset** sufficient to host
freestanding C programs, a self-rebuilding TCC, and a small busybox.
It is **not** SUS-conformant: there is no FPU init, no terminal driver
proper, no permission model, no process groups, no networking, no
pipes/redirects, no threads, no mmap.  The shape is intentionally
POSIX so libc ports (musl, uClibc-ng) drop in cleanly when those gaps
are filled.

## Syscall surface (`int 0x80`, Linux i386 ABI)

### Standard POSIX syscalls present

| # | Name | Status |
|---|---|---|
| 1 | `exit` | Full |
| 2 | `fork` | Full (COW), see `task_fork` in `task.c` |
| 3 | `read` | Full |
| 4 | `write` | Full; fd 1 = VGA, fd 2 = VGA + COM1 (non-POSIX teeing) |
| 5 | `open` | Flags: `O_RDONLY/WRONLY/RDWR` ∨ `O_CREAT 0100` ∨ `O_TRUNC 01000` ∨ `O_APPEND 02000`; `mode` arg ignored |
| 6 | `close` | Full; flushes dirty `FD_KIND_FILE` buffer back to disk |
| 11 | `execve` | Full; `envp` ignored |
| 12 | `chdir` | Full |
| 19 | `lseek` | Full (`SEEK_SET/CUR/END`) |
| 37 | `kill` | Full |
| 45 | `brk` | Full (heap break) |
| 48 | `signal` | Per-signo handler install, returns prev |
| 55 | `fcntl` | Only `F_GETFL` / `F_SETFL` |
| 106 | `stat` | `st_mode/st_size/st_nlink/st_blksize/st_ino` only; perms not enforced |
| 108 | `fstat` | Same shape as `stat` |
| 114 | `wait4` | `WNOHANG`; `rusage` ignored |
| 119 | `sigreturn` | sigframe trampoline; internal |
| 141 | `readdir` | Makar-shaped: `(path, idx, struct dirent *)`, not POSIX `getdents` |
| 158 | `sched_yield` | Full |

### Standard POSIX syscalls **missing**

| Name | Why it's not here | Workaround |
|---|---|---|
| `getpid` / `getppid` | Never wired -- task_t.pid is kernel-visible, ring-3 has no accessor | `/proc/tasks` parse |
| `pipe` / `dup` / `dup2` | Slice 28 not started | -- |
| `mmap` / `munmap` | No anon-mapping ABI, no `PROT_EXEC` | `brk` for heap; ELF loader for code |
| `ioctl` | No device tree past `/dev` block devices | Makar-ext `SYS_PUTCH_AT` etc. |
| `select` / `poll` / `epoll` | Kernel has no fd-readiness model | `SYS_GETKEY` blocks on the focused TTY |
| `access` | -- | `stat` + check mode |
| `mkdir` / `rmdir` / `unlink` / `rename` | Have shell builtins + kernel ext syscalls (`SYS_DELETE_FILE 208`, `SYS_RENAME_FILE 209`, `SYS_DELETE_DIR 210`); no POSIX-named syscalls | Use Makar exts |
| `getuid` / `setuid` / etc | No user model -- everything runs as the implicit root | -- |
| `clock_gettime` / `gettimeofday` | No userspace time API yet | `SYS_UPTIME` (100 Hz ticks) |
| `pthread_*` | No userspace threading; kernel has tasks but no clone | -- |

### Makar extensions (200+)

Documented in `src/kernel/include/kernel/syscall.h`.  Cover terminal
control (`PUTCH_AT`, `SET_CURSOR`, `TTY_CLEAR`, `TERM_SIZE`,
`CARET_STYLE`), framebuffer (`FB_INFO`, `DRAW_LINE`), raw keyboard
(`GETKEY`, `KEYBOARD_RAW`), serial (`WRITE_SERIAL`), FS shortcuts
(`WRITE_FILE`, `LS_DIR`, `DELETE_*`, `RENAME_FILE`, `DISK_INFO`),
shell (`SHELL_CLEAR`, `GETCWD`), and clock (`UPTIME`).  None of these
are POSIX; they exist because Makar doesn't yet have a `termios` /
`ioctl` / `clock_gettime` plumbing path.

## Standard libc

Lives in `src/userspace/` as `malloc.c`, `stdio.c`, `setjmp.S`,
`string.c`, `tcc_compat.c`, archived into `libc.a`.  Headers shipped
to `/usr/include/`.

| Header | Coverage | Gaps |
|---|---|---|
| `<stdio.h>` | `fopen`/`fread`/`fwrite`/`fclose`/`fputs`/`fputc`/`fgetc`/`fflush`/`printf`/`fprintf`/`sprintf`/`snprintf`/`vsnprintf` | No `freopen`, `setbuf`, `tmpfile`, `popen` (no pipes); no wide-char variants |
| `<string.h>` | `memcmp`/`memcpy`/`memmove`/`memset`/`strlen`/`strcpy`/`strncpy`/`strcat`/`strcmp`/`strncmp`/`strchr`/`strrchr`/`strstr`/`strdup` | No `strtok_r`, `strerror`, `strxfrm` |
| `<stdlib.h>` | `malloc`/`free`/`calloc`/`realloc`/`strtol`/`atoi`/`qsort`/`getenv`/`strdup`/`exit`/`abort`/`sscanf` | No `bsearch`, `system`, `mblen`, `wcs*`; no `setenv`/`putenv`/`unsetenv` (env is read-only via `getenv`) |
| `<ctype.h>` | Full ASCII set | No locale awareness (always C locale) |
| `<setjmp.h>` | `setjmp`/`longjmp` | No `sigsetjmp`/`siglongjmp` |
| `<errno.h>` | Minimal; most syscalls return `-1` without setting `errno` | Not POSIX-conformant |
| `<unistd.h>` | Thin syscall wrappers | No `sleep`, `alarm`, `pause`, `pipe`, `dup` |
| `<fcntl.h>` | `O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND` constants only | No `O_NONBLOCK`, `O_SYNC`; no `creat`, `posix_fadvise` |
| `<sys/stat.h>` | `struct stat` with limited fields (see syscall table) | No `mkdir`/`chmod`/`umask` |
| `<dirent.h>` | `struct dirent` via `SYS_READDIR` | No `opendir`/`readdir`/`closedir` *as POSIX defines them* -- API is `readdir(path, idx, &de)` |
| `<signal.h>` | `signal()`, `kill()`, sigframe trampoline | No `sigaction`, `sigprocmask`, `sigsuspend`, `pthread_kill` |
| `<math.h>` | **Absent** | No FPU init in kernel (x87 state not saved across switches) -- fixed-point only |
| `<time.h>` | Minimal | `SYS_UPTIME` ticks only; no `clock_gettime`, `struct tm`, `mktime`, `strftime` |
| `<pthread.h>` | **Absent** | No userspace threads |
| `<locale.h>` / `<wchar.h>` | **Absent** | C locale assumed; no wide chars |

## Shell

`src/kernel/arch/i386/shell/sh_script.c` (in-kernel) and
`src/userspace/sh.c` (ring-3 `sh.elf`) both implement a POSIX-sh
subset.

### Present (POSIX-shaped)

- Variables: `NAME=value`, `$VAR`, `${VAR}`, `$?`, `env`, `unset`
- Control flow: `if`/`elif`/`else`/`fi`, `while`/`do`/`done`, `for V in WORDS; do ... done`
- Tests: `[ ... ]` (string `-z`/`-n`/`=`/`!=`, integer `-eq`/`-ne`/`-lt`/`-le`/`-gt`/`-ge`)
- Builtins: `cd`, `pwd`, `echo`, `exit` (script), `read`, `sleep`, `true`, `false`
- Comments: `# ...`
- Statement separation: `;`, newline
- `$?` reflects: built-ins (always 0), unknown command (127), `[` test (0/1/2), and -- after recent work -- `exec <elf>` child exit status (low 8 bits)

### Absent

| Feature | Notes |
|---|---|
| Command substitution `$(cmd)` / backticks | Not parsed |
| Pipes `\|` | Needs `pipe`/`dup` syscalls -- slice 28 |
| Redirection `<` / `>` / `>>` / `2>` | Same prerequisite as pipes |
| Subshells `( ... )` | Needs `fork`-only-subshell semantics on the in-kernel shell |
| Background jobs `&` + `jobs`/`fg`/`bg` | No job control |
| Functions `name() { ... }` | Not parsed |
| `case ... esac` | Not parsed |
| `getopts`, `trap`, `eval`, `exec` (re-exec self), `export` | -- |
| Quote stripping (`"X"` stays literal `"X"`) | Tokenizer bug; use bare words |
| `IFS`, brace expansion `{a,b}` | -- |

### Path conventions

Linux-shaped: `/` rootfs, `/usr`, `/etc`, `/home`, `/apps`, `/root`,
`/bin`, `/proc`, `/dev`, `/tmp`, `/log`, `/mnt/<name>`,
`/mnt/cdrom`.  No `/sys`, no `/sbin`, no `/var` (except logically
`/log` ≈ `/var/log`).  `/log` is read-only from userspace -- write
via VFS rejects with `"write: read-only filesystem (/log)"`; the
kernel writes its own dmesg ring through `klog_write` which bypasses
the VFS.

## Process / signal model

- Tasks: kernel-scheduled, fixed pool (`MAX_TASKS = 8`).  No clone(2),
  no thread groups.
- pid space: monotonic from 2 (idle = 1).  No pid recycling concerns
  within a session.
- `fork`: COW per-page (`vmm_clone_pd_cow`).
- `execve`: replaces image; auto-transfers keyboard focus + VT
  foreground to the new image.
- `wait4`: blocks, reverse-transfers focus + foreground on reap.
- Signals: per-task `sig_pending` / `sig_mask` bitmasks; `signal()`
  installs handler; sigframe trampoline via `SYS_SIGRETURN`.  Default
  action for unhandled signals: terminate.  SIGSEGV on ring-3 fault
  delivered via `kill_userspace_fault`.
- **No** `sigaction`, `sigprocmask`, process groups, sessions,
  controlling terminal, `tcsetpgrp`, `setsid`, `SIGCHLD` delivery,
  `SIGSTOP`/`SIGCONT`.

## File system

- Permission bits in `st_mode` are returned but **not enforced** --
  every task has implicit root-like access.
- No uid / gid in `struct stat` (always 0).
- No timestamps (`st_atime` / `mtime` / `ctime` not implemented).
- No symlinks, no hard links beyond what FAT32 / ext2 / ISO9660
  natively expose.
- `readdir(path, idx, ...)` is iterative-index, not POSIX-shaped
  opaque `DIR *`.
- `/tmp` (tmpfs) has wholesale-overwrite semantics on
  `vfs_write_file` (matches POSIX `O_TRUNC` close-flush expectations).
- `/log` (logfs) is ring-append; user-write rejected by the VFS.

## What an app porter needs to know

If you're bringing a POSIX-flavoured app over:

1. **No floats.**  Use fixed-point or refactor to integer math.
2. **No `pipe` / redirection.**  Restructure to single-process; use
   intermediate temp files in `/tmp` if needed.
3. **No `mmap`.**  Stick to `brk`-based malloc; the libc shim does
   this transparently.
4. **No real `errno`.**  Check syscall return for `-1`; cause-of-failure
   is in serial log, not user-facing.
5. **`signal()` works, but no `sigaction`.**  Re-install handler in
   the handler if you need persistent semantics.
6. **No threads.**  If the app needs concurrency, `fork`+pipe (after
   slice 28) is the route -- or restructure to event-driven on
   `SYS_GETKEY`.
7. **Use Makar exts for terminal control.**  No `termios` --
   `SYS_TERM_SIZE`, `SYS_SET_CURSOR`, `SYS_PUTCH_AT`, `SYS_CARET_STYLE`
   are the surfaces.
8. **`/log` is RO.**  Write scratch files to `/tmp`.

See `src/userspace/alloctest.c` for a near-comprehensive smoke test
of what works (12 sub-tests across the libc surface).
