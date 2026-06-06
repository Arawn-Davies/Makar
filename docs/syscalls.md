---
title: Syscall ABI
parent: Reference
nav_order: 2
---

# Syscall ABI

Makar uses the Linux i386 syscall calling convention:

```text
int 0x80
EAX = syscall number
EBX = arg0
ECX = arg1
EDX = arg2
ESI = arg3
EDI = arg4
EAX = return value
```

The authoritative kernel definitions live in:

```text
src/kernel/include/kernel/syscall.h
```

The userspace inline wrappers live in:

```text
src/userspace/syscall.h
```

Keep those files synchronized. Userspace objects now use generated dependency
files so changes to `syscall.h` rebuild dependent apps.

## Return Conventions

Makar mostly follows the Unix pattern:

- non-negative return values indicate success
- `-1` or negative errno-style values indicate failure depending on the syscall
- userspace libc wrappers are gradually being moved toward setting `errno`

Some older Makar-specific syscalls still return a simple `0`/`1` or `-1`
without a detailed errno. Check the wrapper before assuming Linux parity.

## Core Linux-Compatible Syscalls

| Number | Name | Arguments | Status |
|---:|---|---|---|
| 1 | `SYS_EXIT` | `status` | terminates current task |
| 2 | `SYS_FORK` | none | COW fork; parent gets child pid, child gets 0 |
| 3 | `SYS_READ` | `fd, buf, len` | fd-backed read |
| 4 | `SYS_WRITE` | `fd, buf, len` | fd-backed write |
| 5 | `SYS_OPEN` | `path, flags, mode` | mode ignored |
| 6 | `SYS_CLOSE` | `fd` | closes and flushes |
| 10 | `SYS_UNLINK` | `path` | delete file |
| 11 | `SYS_EXECVE` | `path, argv, envp` | envp ignored |
| 12 | `SYS_CHDIR` | `path` | per-task cwd |
| 19 | `SYS_LSEEK` | `fd, offset, whence` | `SEEK_SET/CUR/END` |
| 20 | `SYS_GETPID` | none | current task pid |
| 37 | `SYS_KILL` | `pid, signo` | signal delivery |
| 38 | `SYS_RENAME` | `old, new` | VFS rename |
| 39 | `SYS_MKDIR` | `path, mode` | mode ignored |
| 40 | `SYS_RMDIR` | `path` | remove empty directory |
| 41 | `SYS_DUP` | `oldfd` | duplicate to lowest free fd |
| 42 | `SYS_PIPE` | `int pipefd[2]` | creates read/write fds |
| 45 | `SYS_BRK` | `addr` | query/grow heap break |
| 48 | `SYS_SIGNAL` | `signo, handler` | simple signal handler install |
| 54 | `SYS_IOCTL` | `fd, request, ...` | compatibility stub, returns `-ENOTTY` |
| 55 | `SYS_FCNTL` | `fd, cmd, arg` | `F_GETFL`, `F_SETFL` |
| 63 | `SYS_DUP2` | `oldfd, newfd` | duplicate onto requested fd |
| 64 | `SYS_GETPPID` | none | parent pid |
| 78 | `SYS_GETTIMEOFDAY` | `timeval *, tz` | tz ignored |
| 91 | `SYS_MUNMAP` | `addr, len` | unmap anonymous pages |
| 106 | `SYS_STAT` | `path, stat *` | limited Linux-shaped stat |
| 108 | `SYS_FSTAT` | `fd, stat *` | limited Linux-shaped stat |
| 114 | `SYS_WAIT4` | `pid, status *, options, rusage` | supports `WNOHANG`; rusage ignored |
| 119 | `SYS_SIGRETURN` | internal | signal trampoline return |
| 141 | `SYS_READDIR` | `path, index, dirent *` | indexed Makar syscall, libc wraps it |
| 158 | `SYS_YIELD` | none | scheduler yield |
| 175 | `SYS_RT_SIGPROCMASK` | Linux args | startup compatibility stub |
| 192 | `SYS_MMAP2` | `addr, len, prot, flags, fd, pgoff` | anonymous only |
| 240 | `SYS_FUTEX` | Linux args | single-threaded compatibility stub |
| 243 | `SYS_SET_THREAD_AREA` | `user_desc *` | one-slot i386 TLS |
| 252 | `SYS_EXIT_GROUP` | `status` | same as exit |
| 258 | `SYS_SET_TID_ADDRESS` | `int *` | returns pid |
| 265 | `SYS_CLOCK_GETTIME` | `clockid, timespec *` | realtime and monotonic |

## Makar Extension Ranges

Makar extensions provide functionality that would normally be handled by
termios, ioctl, framebuffer drivers, devfs, procfs, or shell helpers on a
larger Unix system.

The exact list is in `src/kernel/include/kernel/syscall.h`; the groups below
explain the design intent.

### Terminal and Framebuffer

These support text-mode and framebuffer apps without a termios layer:

- put a character/cell at a position
- move the cursor
- clear the terminal
- query terminal size
- query framebuffer geometry
- draw framebuffer lines
- set caret style
- clear using shell/default colors

Display-mutating syscalls are focus-gated where appropriate so background VTs
update backing buffers without scribbling on the visible framebuffer.

### Shared Pixel Surfaces

The only shared-memory primitive in the system (`fork` is copy-on-write and
`SYS_MMAP2` is private-anon only). A surface is a kernel-owned run of physical
frames that can be mapped into more than one task at the same physical
location, so a window manager and a forked graphical child (e.g. `doom.elf`
windowed) can share a frame buffer: the WM composites what the child renders.
See `kernel/surface.h` for the lifetime model.

| Number | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 259 | `SYS_SURFACE_CREATE` | `w, h` | surface id (`>=0`) holding a creator ref, or `-1` |
| 262 | `SYS_SURFACE_MAP` | `id` | base user address, or `0`/`NULL` on failure |
| 263 | `SYS_SURFACE_INFO` | `id` | `(w << 16) | h`, or `-1` for a bad id |
| 264 | `SYS_SURFACE_DESTROY` | `id` | drop the creator ref; `0` ok, `-1` error |

A surface stays alive while its creator reference is held **or** any task still
has it mapped; its frames are reclaimed once neither holds. On task teardown
the kernel unmaps the dying task's surface pages **before** its page directory
is torn down, so the shared frames are never double-freed.

### Keyboard

Keyboard syscalls expose:

- blocking key reads
- raw keyboard mode
- focus-aware routing

Automated keyboard tests do not use a syscall. The kernel test driver injects
keycodes directly into the live keyboard path when `kbtest` is on the command
line.

### Filesystem Convenience

Makar provides shortcut syscalls for:

- write an entire file
- list a directory into a text buffer
- delete files/directories
- rename/move paths
- disk information
- PCI information
- statfs-like rootfs usage

These are not POSIX ABI surfaces; they exist for small built-in tools and
apps.

### Networking

Networking currently uses a monolithic lwIP integration over the kernel
`netdev` layer. The public userspace surface is intentionally small:

| Syscall | Number | Purpose |
|---|---:|---|
| `SYS_NET_INFO` | 253 | render active Ethernet/lwIP state into a caller buffer |
| `SYS_NET_CTL` | 254 | run a network control command |
| `SYS_WGET` | 255 | fetch an `http://` URL (EBX) and write the body to a VFS path (ECX); returns bytes saved, or negative on error (`-(status)` for a non-2xx reply) |

`SYS_NET_CTL` accepts these command values from `src/userspace/syscall.h`:

| Command | Value | Behavior |
|---|---:|---|
| `NET_CTL_DHCP_RELEASE` | 1 | release/stop DHCP and restore static QEMU slirp fallback addressing |
| `NET_CTL_DHCP_RENEW` | 2 | restart DHCP, then fall back static if no lease arrives quickly |
| `NET_CTL_DNS_FLUSH` | 3 | clear lwIP DNS cache and pending resolver requests |

The user-facing tool is `/apps/maknetcfg.elf`; see
[Networking](networking.md).

### Session and Admin

Admin/session helpers include:

- reboot
- shutdown
- display mode changes
- foreground/background color selection
- mount/umount/mkfs/eject/install helpers
- scheduler quantum controls
- serial verbose toggle
- hostname and username queries

There is no permission model yet, so privilege checks are structural rather
than user/credential based.

| Syscall | Args | Returns |
|---|---|---|
| 260 `SYS_LOGOUT` | — | end the root login session; `0` ok, `-1` |
| 266 `SYS_LOGIN` | `user, pass` | `shadow_verify` + set session user; `0` ok, `-1` bad creds |

`SYS_LOGIN` backs the GUI graphical login (`gui.elf`'s `do_login`); see
`docs/gui.md`.

### Virtual Terminals and makmux

VT/app-tab syscalls support the userspace multiplexer:

| Syscall | Purpose |
|---|---|
| `SYS_VT_ENTER` | bind a task to a new VT slot |
| `SYS_VT_CLOSE` | close/free a VT owner pid |
| `SYS_VT_OPEN_REQUEST` | request another shell VT |
| `SYS_VT_STATE` | return live VT state |
| `SYS_VT_OPEN_APP` | queue or switch to a named app tab |
| `SYS_VT_TAKE_APP` | makmux drains one queued app path |
| `SYS_VT_SETNAME` | name the current VT/app tab |
| `SYS_VT_GETNAME` | read a VT/app tab name |

`SYS_VT_TAKE_APP` is currently number 248. It was moved off 243 so Linux i386
`set_thread_area` could use its standard number.

## TLS Details

`SYS_SET_THREAD_AREA` accepts a Linux-style i386 `struct user_desc` pointer.
Makar supports one TLS slot:

- GDT index: 6
- selector: `0x33`
- writeback entry number: 6
- segment register: `%gs`

The syscall programs the GDT slot, records the TLS descriptor on the current
task, and loads `%gs`. ISR and IRQ stubs intentionally leave `%gs` untouched.
The scheduler restores TLS state for TLS-active tasks.

## mmap Details

`SYS_MMAP2` is intentionally narrow:

- requires `MAP_ANONYMOUS`
- rejects `MAP_FIXED`
- ignores file descriptors and offsets
- maps zero-filled pages
- uses a per-task bump pointer
- returns `MAP_FAILED` (`(void *)-1`) on unsupported requests

`SYS_MUNMAP` unmaps pages but does not currently recycle virtual addresses.

## Compatibility Stubs

These exist for hosted libc startup, especially static musl:

| Syscall | Behavior |
|---|---|
| `ioctl` | returns `-ENOTTY` |
| `rt_sigprocmask` | returns success |
| `futex` | returns success under the current single-threaded assumption |
| `set_tid_address` | returns pid |
| `exit_group` | exits current process |

Treat them as bring-up compatibility, not complete Linux behavior.
