---
title: Syscall ABI
parent: Reference
nav_order: 2
---

# Syscall ABI (`int 0x80`, Linux i386 convention)

Makar follows the Linux i386 syscall convention: `int 0x80`, EAX = syscall number, arguments in EBX/ECX/EDX/ESI/EDI, return value in EAX.

Authoritative number assignments: `src/kernel/include/kernel/syscall.h`.  
Userspace wrappers (inline stubs): `src/userspace/syscall.h`.

## Standard POSIX-shaped syscalls

| EAX | Name | Args / notes |
|-----|------|-------------|
| 1 | `SYS_EXIT` | EBX = status. Sets `task_current()->exit_status` before ZOMBIE/DEAD. |
| 2 | `SYS_FORK` | COW-clone the calling task; returns child pid in parent, 0 in child, -EAGAIN on failure. |
| 3 | `SYS_READ` | EBX = fd (0 = stdin keyboard, ≥3 = VFS), ECX = buf, EDX = count. |
| 4 | `SYS_WRITE` | EBX = fd, ECX = buf, EDX = count. fd 1 = VGA, fd 2 = VGA + COM1, ≥3 = VFS. Dirty `FD_KIND_FILE` buffers flushed on close. |
| 5 | `SYS_OPEN` | EBX = path, ECX = flags (`O_RDONLY/WRONLY/RDWR` ∣ `O_CREAT 0100` ∣ `O_TRUNC 01000` ∣ `O_APPEND 02000`; Linux i386 values). `/dev` block devices bind as `FD_KIND_BLOCKDEV`; other paths eager-buffer up to 16 MiB. |
| 6 | `SYS_CLOSE` | EBX = fd. Flushes dirty `FD_KIND_FILE` buffer via `vfs_write_file`; frees buffer regardless. |
| 10 | `SYS_UNLINK` | EBX = path. POSIX alias of SYS_DELETE_FILE (208). |
| 11 | `SYS_EXECVE` | EBX = path, ECX = argv (NULL-terminated), EDX = envp (ignored). On success never returns. Transfers keyboard focus + VT foreground to new image. |
| 12 | `SYS_CHDIR` | EBX = path. Sets calling task's cwd (normalises `../` and `//`). Returns 0/-1. |
| 19 | `SYS_LSEEK` | EBX = fd, ECX = offset, EDX = whence. Works on `FD_KIND_FILE` and `FD_KIND_BLOCKDEV`. |
| 20 | `SYS_GETPID` | Returns `task_current()->pid` (idle task = 1). |
| 37 | `SYS_KILL` | EBX = pid, ECX = signo. |
| 38 | `SYS_RENAME` | EBX = old, ECX = new. POSIX alias of SYS_RENAME_FILE (209). |
| 39 | `SYS_MKDIR` | EBX = path, ECX = mode (ignored). Calls `vfs_mkdir`. |
| 40 | `SYS_RMDIR` | EBX = path. POSIX alias of SYS_DELETE_DIR (210). |
| 42 | `SYS_PIPE` | EBX = `int pipefd[2]` (out). Allocates two fds backed by a shared 4 KiB `pipe_ring_t`. Returns 0/-1. |
| 45 | `SYS_BRK` | EBX = new break. Returns current/new break. |
| 48 | `SYS_SIGNAL` | EBX = signo, ECX = handler. Returns previous handler. |
| 63 | `SYS_DUP2` | EBX = oldfd, ECX = newfd. Closes newfd if open; for `FD_KIND_PIPE` bumps the correct refcount. Returns newfd/-1. |
| 64 | `SYS_GETPPID` | Returns `task_current()->parent_pid` (0 = no userspace ancestor). |
| 78 | `SYS_GETTIMEOFDAY` | EBX = `struct timeval *`, ECX = `struct timezone *` (ignored). `tv_sec` from CMOS RTC; `tv_usec` resolution = 10 ms (PIT tick). |
| 100 | `SYS_DEBUG` | EBX = uint32 checkpoint. Prints to VGA + serial unconditionally. |
| 106 | `SYS_STAT` | EBX = path, ECX = `struct stat *`. Populates `st_mode`/`st_size`/`st_nlink`/`st_blksize`/`st_ino` (FNV-1a-32 hash of path). |
| 108 | `SYS_FSTAT` | EBX = fd, ECX = `struct stat *`. Same shape; for `FD_KIND_FILE`, `st_size` reflects unflushed writes. |
| 114 | `SYS_WAIT4` | EBX = pid (-1 = any child), ECX = `int *status`, EDX = options (WNOHANG=1), ESI = rusage (ignored). Returns child pid, 0 (WNOHANG/no zombie), or -ECHILD. On reap, keyboard focus + VT foreground return to parent. |
| 119 | `SYS_SIGRETURN` | Sigframe trampoline — not for direct use. |
| 158 | `SYS_YIELD` | Voluntary scheduler yield. |
| 265 | `SYS_CLOCK_GETTIME` | EBX = clockid (`CLOCK_REALTIME=0`, `CLOCK_MONOTONIC=1`), ECX = `struct timespec *`. 10 ms resolution. |

## TTY / display syscalls

| EAX | Name | Args / notes |
|-----|------|-------------|
| 200 | `SYS_GETKEY` | Raw single-char keyboard read (blocks until key available). |
| 201 | `SYS_PUTCH_AT` | EBX = `tty_cell_t[]`, ECX = count. Batch cell write for fullscreen apps. Cells at `row >= drawable_rows` target the makmux status bar directly. |
| 202 | `SYS_SET_CURSOR` | EBX = col, ECX = row. |
| 203 | `SYS_TTY_CLEAR` | EBX = VGA colour attribute. Clear screen with background fill. |
| 204 | `SYS_TERM_SIZE` | Returns `(cols << 16) \| rows`. Rows = full screen when no makmux VTs registered; `rows-1` (status bar reserved) when makmux is active. |
| 211 | `SYS_WRITE_SERIAL` | EBX = buf, ECX = len. COM1 only, no framebuffer. |
| 212 | `SYS_KEYBOARD_RAW` | EBX = 1 = raw bytes (no sentinel translation), 0 = cooked. |
| 213 | `SYS_SHELL_CLEAR` | Same as `clear` shell builtin. |
| 214 | `SYS_UPTIME` | Returns 100 Hz PIT tick counter. |
| 215 | `SYS_GETCWD` | EBX = char *buf, ECX = size. Returns strlen or -1. |
| 217 | `SYS_DRAW_LINE` | EBX = (x0<<16)\|y0, ECX = (x1<<16)\|y1, EDX = 24-bit RGB. Clipped to drawable area (excludes status row when makmux active). |
| 218 | `SYS_CARET_STYLE` | EBX = style (0 = line, 2 = flashing block). Returns previous. No-op in VGA-text mode. |

## VFS / disk syscalls

| EAX | Name | Args / notes |
|-----|------|-------------|
| 205 | `SYS_WRITE_FILE` | EBX = path, ECX = buf, EDX = len. |
| 206 | `SYS_LS_DIR` | EBX = path, ECX = buf, EDX = bufsz. |
| 207 | `SYS_DISK_INFO` | EBX = buf, ECX = bufsz. |
| 208 | `SYS_DELETE_FILE` | EBX = path. |
| 209 | `SYS_RENAME_FILE` | EBX = old, ECX = new. |
| 210 | `SYS_DELETE_DIR` | EBX = path. |

## VT multiplexer syscalls (makmux)

| EAX | Name | Args / notes |
|-----|------|-------------|
| 233 | `SYS_VT_ENTER` | EBX = focus_new. Register calling task as a makmux VT child, name it `mak.shN`. Returns slot index or -1. |
| 234 | `SYS_VT_CLOSE` | EBX = pid. Unregister a VT child by pid. |
| 235 | `SYS_VT_OPEN_REQUEST` | Consume pending Alt+T "open new VT" counter. Returns count consumed. |
| 236 | `SYS_VT_STATE` | Returns `(active_slot << 16) \| live_mask`. |
| 237 | `SYS_VT_CLOCK_REQUEST` | Consume pending Alt+F5 clock-toggle requests. |

## Keyboard switching (host-key reference)

| Shortcut | Action |
|----------|--------|
| Alt+F1..F4 | Switch to VT 1..4 |
| Ctrl+Tab | Cycle to next VT |
| Ctrl+Shift+Tab | Cycle to previous VT |
| Alt+F5 | Toggle clock mode in makmux status bar |
| Alt+T | Open a new VT (makmux must be running) |
| Ctrl+A, U/J | Pane switch (legacy in-kernel shortcut) |
| Ctrl+C | SIGINT to focused task / abort input line |
