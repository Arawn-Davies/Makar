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

The ABI is **shared verbatim** between kernel and userspace (Linux-uapi style).
The authoritative definitions live in:

```text
src/kernel/include/makar_syscalls.h   # the SYS_* numbers
src/kernel/include/makar_abi.h         # clockids, struct timeval/timespec/stat/dirent,
                                       # tty_cell_t, open/fcntl flags, S_IF* + DT_* bits
src/kernel/include/makar_signals.h     # signal numbers
src/kernel/include/makar_keys.h        # keycodes / sentinels
```

Both `kernel/syscall.h` (kernel-internal dispatch) and `src/userspace/syscall.h`
(the ring-3 inline wrappers) **include** those headers — they never re-declare
the numbers, so the two sides cannot drift. Userspace objects use generated
dependency files, so editing the ABI headers rebuilds the dependent apps.

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
| 5 | `SYS_OPEN` | `path, flags, mode` | mode ignored; read-only disk files are lazy (page-cached) |
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
| 45 | `SYS_BRK` | `addr` | query/grow heap break (demand-paged) |
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
| 192 | `SYS_MMAP2` | `addr, len, prot, flags, fd, pgoff` | anonymous only, demand-paged |
| 240 | `SYS_FUTEX` | Linux args | single-threaded compatibility stub |
| 243 | `SYS_SET_THREAD_AREA` | `user_desc *` | one-slot i386 TLS |
| 252 | `SYS_EXIT_GROUP` | `status` | same as exit |
| 258 | `SYS_SET_TID_ADDRESS` | `int *` | returns pid |
| 265 | `SYS_CLOCK_GETTIME` | `clockid, timespec *` | realtime and monotonic |
| 274 | `SYS_VIDEO_CAPS` | – | returns `VIDEO_CAP_*` of the active display driver |
| 275 | `SYS_HWCURSOR_DEFINE` | `argb, (w<<16)|h, (hx<<16)|hy` | upload a HW cursor sprite |
| 276 | `SYS_HWCURSOR_MOVE` | `x, y` | move the HW cursor overlay |
| 277 | `SYS_HWCURSOR_SHOW` | `on` | show/hide the HW cursor |
| 278 | `SYS_VIDEO_NAME` | `buf, cap` | copy the active video backend's name into `buf` |

## Makar Extension Ranges

Makar extensions provide functionality that would normally be handled by
termios, ioctl, framebuffer drivers, devfs, procfs, or shell helpers on a
larger Unix system.

The exact list is in `src/kernel/include/makar_syscalls.h`; the groups below
explain the design intent.

### Terminal and Framebuffer

These support text-mode and framebuffer apps without a termios layer:

- put a character/cell at a position
- move the cursor
- clear the terminal
- query terminal size
- query framebuffer geometry
- draw framebuffer lines
- present a full-frame back buffer (`SYS_FB_PRESENT`, 257) or just a sub-rect of
  it (`SYS_FB_PRESENT_RECT`, 269 — lets the compositor refresh the cursor box
  without re-pushing the whole frame)
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

### IPC (synchronous message passing)

MINIX-style rendezvous: endpoints are task pids, messages are fixed 32-byte
`ipc_msg_t` (a `type` opcode + 6 data words), transfer is synchronous (a send
blocks until the destination is in a matching receive). `ebx` = endpoint pid (or
`IPC_ANY = -1` for receive), `ecx` = `ipc_msg_t *`. See `kernel/ipc.h`. This is
the control channel for the makx display server/client split (pixels go over a
shared surface); `docs/gui.md` describes the makx protocol layered on top.

| Number | Name | Arguments | Returns |
| --- | --- | --- | --- |
| 249 | `SYS_IPC_SEND` | `dst, msg` | `0` ok, `-ESRCH`/`-EINVAL`/`-EFAULT` |
| 250 | `SYS_IPC_RECV` | `from, out` | `0` ok (blocks until a sender), or `-err` |
| 251 | `SYS_IPC_SENDREC` | `dst, msg` | atomic send-then-await-reply (RPC) |
| 267 | `SYS_IPC_NBRECV` | `from, out` | `0` if a message was queued, else `-EAGAIN` (`-11`); never blocks |

`SYS_IPC_NBRECV` lets a server (the makx display server) drain queued client
requests in the same loop it polls hardware input, instead of parking in a
blocking `ipc_recv`.

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
| 270 `SYS_PASSWD` | `old, new` | change the current session user's password (`shadow_verify` old + `shadow_set_password` new); `0` ok, `-2` wrong current password, `-1` otherwise (bad args / read-only live rootfs) |
| 271 `SYS_CAD_PENDING` | — | test-and-clear the Ctrl-Alt-Del flag; `1` if it was pressed. The GUI server polls it each frame to open its power menu |
| 272 `SYS_PTY_WINSIZE` | `fd, (cols<<16)\|rows` | publish a pty window size onto a pipe fd so a piped child's `SYS_TERM_SIZE` reports it (GUI terminal → shell); `0` ok, `-1` if not a pipe |
| 273 `SYS_INSTALL_EXEC` | `cmd, ptr` | stepped headless installer for the GUI front-end (`mxinstall.elf`). `cmd`: `0` begin(`install_params_t*`), `1` step(`install_progress_t*`, copies one file), `2` finish(`install_params_t*`), `3` list-drives(`install_drive_t[4]`). Returns the engine result (see `kernel/installer.h`). Runs preemptibly so the compositor stays live during the copy |

`SYS_LOGIN` backs the GUI graphical login (`gui.elf`'s `do_login`); `SYS_PASSWD`
backs its change-password dialog (the power menu's "Change password..."). Both
are documented in `docs/gui.md`.  `SYS_INSTALL_EXEC` backs the graphical
installer `mxinstall.elf`; the text installer (`install` builtin →
`SYS_INSTALL`) and the GUI share one execution engine (see `docs/gui.md`).

### Virtual Terminals and makmux

VT/app-tab syscalls support the userspace multiplexer:

| Syscall | Purpose |
|---|---|
| `SYS_VT_ENTER` | bind a task to a new VT slot |
| `SYS_VT_CLOSE` | close/free a VT owner pid |
| `SYS_VT_OPEN_REQUEST` | request another shell VT (the Alt+T route) |
| `SYS_VT_STATE` | return live VT state: `(focused_slot << 16) \| live_mask` |
| `SYS_VT_OPEN_APP` | queue or switch to a named app tab |
| `SYS_VT_TAKE_APP` | makmux drains one queued app path |
| `SYS_VT_SETNAME` | name the current VT/app tab |
| `SYS_VT_GETNAME` | read a VT/app tab name |

`SYS_VT_TAKE_APP` is currently number 248. It was moved off 243 so Linux i386
`set_thread_area` could use its standard number.

There are **nine user-visible VT slots** (kernel slots 0–8) plus a hidden root
console (`VTTY_ROOT_SLOT`). They are exposed to userspace as Linux-style
character nodes under `/dev`:

| Path | Slot | Notes |
|---|---|---|
| `/dev/tty0` | `VTTY_ROOT_SLOT` | the root console (mak.sh0), hidden from the tab strip |
| `/dev/tty1` … `/dev/tty9` | 0 … 8 | the nine makmux VT slots (slot == ttyN − 1) |

These nodes are **always present** (independent of makmux). They are character
sinks, not block devices: reads return EOF, and a write streams into that
slot's backing grid (`vtty_write`), painting the framebuffer if the VT is
focused — e.g. `echo hi > /dev/tty2`. No new syscall: writes ride the existing
`SYS_OPEN`/`SYS_WRITE` block-device fd path through devfs. makmux is tmux-style
— it opens **one** shell by default and grows more on the `SYS_VT_OPEN_REQUEST`
(Alt+T) route, up to nine.

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
- **demand-paged**: only reserves the range (advances a per-task bump pointer);
  zero-filled frames are mapped on first touch by the page-fault handler, the
  same lazy model Linux uses. A large mapping therefore costs O(1) in the call
  rather than mapping every page up front.
- returns `MAP_FAILED` (`(void *)-1`) on unsupported requests

`SYS_BRK` is likewise lazy: growing the break only advances `user_brk`; pages in
`[user_brk_base, user_brk)` fault in zeroed on first touch. (Query/shrink-to
forms behave as before; the break only ever grows.)

`SYS_MUNMAP` unmaps pages but does not currently recycle virtual addresses.
Because the mmap window is a grow-only bump arena, touching a munmap'd address
below `mmap_next` simply re-faults a fresh zero page rather than `SIGSEGV`-ing —
acceptable for the current bring-up (no address reuse).

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
