# Makar roadmap (Claude reference)

Companion to `CLAUDE.md`. Forward-looking plans and the slice queue. Not needed for
day-to-day work — consult when planning new features or asked about direction.

## Future roadmap

### Slice queue (`feat/tty-multitasking` → follow-ups)

Tracked here, pulled into branches one at a time so each PR stays focused.

#### Done

| # | Slice | Status |
|---|---|---|
| 1 | **Reaper for dead-task user PDs** | ✅ shipped (`fcb8771`) |
| 2 | **Per-task `task_t` plumbing** (pid/cwd/tty/fds/signals fields, no consumer migration) | ✅ shipped (`3a0ef78`) |
| 3 | **Ring-3 lifecycle ktest** with serial proof | ✅ shipped (`f48d730`, `1a34c20`) |
| 4 | **100 Hz timer + humanised uptime** + stderr→serial + `SYS_WRITE_SERIAL` | ✅ shipped (`5e40001`) |
| 5 | **Keyboard rewrite** — full PS/2 set-1 + e0, layered decoder (scancode→keycode→ASCII/sentinel→router), IRQ-driven per-TTY rings, strict make/break separation, `unsigned char` end-to-end | ✅ shipped (#124) |
| 6 | **Keyboard hardening** — `unsigned char` audit, typematic-repeat filter for modifiers, PS/2 LED sync, boot-time LED state read | ✅ shipped (#127) |
| 7 | **Test-infra cleanup** — ccache, single-kernel/two-ISO, build-once fan-out CI, KVM gate | ✅ shipped (#125) |
| 8 | **Per-task consumer migration** — `vtty` routes via `task->tty` authoritatively; fd-table consumer migration (see slice 12); cwd authoritative (see slice 13). | ✅ |
| 9 | **Linux-style signal subsystem** — per-task handler table + scheduler-driven default-terminate delivery; `SYS_KILL(37)` + `SYS_SIGNAL(48)` + `SYS_SIGRETURN(119)` + ring-3 trampoline; Ctrl+C → SIGINT migration; `sigtest.elf` + `user-sigusr1-handler` ui scenario.  Remaining polish: interactive signal picker. | ✅ |
| 10 | **Preemption hardening** — `in_schedule` re-entrancy guard + IRQ-save around `schedule()`; per-task `kticks` in `/proc/tasks`; runtime-tunable `g_sched_quantum` via `sched_quantum` shell builtin; `test_preempt` ktest. | ✅ |
| 11 | **Per-TTY screen buffers + /proc** — `vt_buf_t` backing grid per TTY, deferred FB repaint on Alt+Fn, tmux-style status bar, synthetic `/proc` with `cpuinfo` / `meminfo` / `tasks` / `uname`. | ✅ shipped (#129) |
| 12 | **Per-task FD table** — real `fd_table_t` (kernel/fd.h); fds 0/1/2 pre-bound; SYS_READ/WRITE/OPEN/CLOSE/LSEEK route through the calling task's table.  Foundation for pipe/dup and fork's fd dup. | ✅ |
| 13 | **VFS `task->cwd` authoritative** — `s_cwd` global removed from `vfs.c`; relative paths resolved against `task_current()->cwd`. | ✅ |
| 14 | **makbox multicall + `SYS_GETCWD` + exec race fix** — busybox-style `makbox.elf` (ls/cat/cp/mv/rm/rmdir/echo/pwd); `SYS_GETCWD(215)`; per-task `exec_params` (closes the cross-TTY `CS=0x3F8` exec race); ui-test refactored to shared-VM runner. | ✅ |
| 15 | **fork() + COW** — full Linux-style fork with copy-on-write.  See 15a..15e. | ✅ |
| 15a | **PMM frame refcounts** — per-frame `uint8_t refcount` table; `pmm_alloc_frame` sets refcount=1, `pmm_free_frame` decrements and only releases at 0; `pmm_inc_ref` / `pmm_ref_count`.  Preserves single-owner caller behaviour. | ✅ |
| 15b | **`vmm_clone_pd_cow()`** — walk parent PD, mark user PTEs RO + software COW bit (`VMM_PTE_COW`, PTE bit 9), `pmm_inc_ref` each frame, mirror into child PD; CR3 reload if parent active. | ✅ |
| 15c | **COW `#PF` handler + `CR0.WP`** — write-fault on COW-tagged PTE: refcount==1 fast-path (clear COW + set RW), else alloc fresh frame + memcpy + dec old refcount.  `CR0.WP` enabled at `paging_init` so kernel writes honour user RO bits. | ✅ |
| 15d | **`SYS_FORK` (EAX=2) + `fork_child_iret`** — `task_fork()` clones task slot, dups fd_table, inherits cwd/tty/user_brk, clones PD via 15b, resets sig handlers; child's kstack hand-built so first `task_switch` ret lands in `fork_child_iret` (mirror of isr_common_stub epilogue) and iret's to ring 3 with EAX=0. | ✅ |
| 15e | **Multi-page COW + exit-code coverage** — `forktest.elf` exercises five independent BSS sentinels (single + 4 page-aligned), proves COW visibility + isolation across multiple pages; child exits with status=42 to confirm kernel logs propagation. | ✅ |
| 16 | **execve + wait4** — POSIX child reaping completes the fork+exec story.  See 16a..16b. (local-only on `feat/its-posix-bitch`, not yet pushed.) | ✅ local |
| 16a | **`SYS_EXECVE` (EAX=11)** — replaces caller's address space with a new ELF; `elf_exec` frees the old user PD on success; argv copied into kernel scratch before PD swap; sig handlers reset per POSIX.  `execvetest.elf` does fork→child-execve→parent-survives. | ✅ local |
| 16b | **`SYS_WAIT4` (EAX=114) + `TASK_ZOMBIE` state** — child holds its pool slot + `exit_status` until parent reaps it; `parent_pid` + `exit_status` on `task_t`; orphan zombies auto-reaped by parent's `task_exit`.  `forktest.elf` + `execvetest.elf` now wait4 instead of busy-yielding. | ✅ |
| 22 | **Ring-3 page-fault → SIGSEGV** — `kill_userspace_fault` in debug.c logs offending pid+name+EIP, sets exit_status, calls `task_exit`.  Panic screen for ring-0 faults now identifies the running task.  Userspace null-derefs no longer take down the kernel. | ✅ shipped (v0.8, `feat/tcc-shell`) |
| 23 | **Zsh-style tab cycling** — first Tab on ambiguous prefix extends to longest common prefix; subsequent Tabs cycle matches in place; any non-Tab key commits.  `tc_active`/`tc_idx`/`tc_matches[]` state in `shell_readline`. | ✅ shipped (v0.8) |
| 24 | **HDD root layout** — `resolve_rootfs_prefix` probes for `/usr/lib/crt0.o` and elevates the holding mount to `/`; a non-empty mount named "boot" is elevated to `/boot`; `vfs_route` rewrites `/usr`, `/etc`, `/home`, `/src`, etc. via the rootfs prefix; `ls /mnt` filters elevated mounts.  Linux-style namespace; synthetic overlays (`/proc`, `/dev`, `/log`, `/tmp`, `/mnt`) untouched. | ✅ shipped (v0.8) |
| 25 | **In-OS TCC self-host milestone (v0.8 headline)** — `tcc.elf` ships on every ISO; `tcc /src/userspace/calc.c -o /tmp/calc.elf` and `tcc /src/userspace/sh.c -o /tmp/sh.elf` rebuild correct binaries in-OS.  Fixed upstream NULL-deref in `vendor/tinycc/tccelf.c:fill_local_got_entries`.  `SYSCALL_FILE_MAX` 8 → 16 MiB; kernel heap 16 → 32 MiB.  Tests: `test_tcc_hello`, `test_tcc_hello_relpath`, `test_tcc_rebuild_calc`, `test_tcc_rebuild_sh`. | ✅ shipped (v0.8) |
| 26 | **Ring-3 shell MVP** — `/apps/sh.elf` is a freestanding ring-3 shell (only `#include "syscall.h"`) with prompt, line input, tokenize, `cd`/`pwd`/`exit` builtins, fork/execve/wait4 dispatch.  SYS_CHDIR(12) added.  SYS_EXECVE auto-transfers keyboard focus + VT foreground to the new image; SYS_WAIT4 reverses the transfer on child reap.  Coexists with the in-kernel shell — opt-in via `exec /apps/sh.elf`.  Tests: `test_usershell_smoke`, `test_usershell_execve`, `test_tcc_rebuild_sh`. | ✅ shipped (v0.8) |

#### Open

| # | Slice | Status |
|---|---|---|
| 17 | **UTF-8 terminal** with ASCII fallback / runtime mode switch | ⏭ deferred |
| 18 | **`ps`-style task listing** with privilege/state/CWD/TTY columns (today's `cat /proc/tasks` covers this; promote only if richer column control is needed) | ⏭ deferred |
| 19 | **VGA-fallback per-TTY** — route `tty.c` writes through `vt_buf` so VGA-text mode gets the same per-TTY isolation that VESA already has | ⏭ |
| 20 | **Userland shell (full parity, multi-PR feature)** — lift the in-kernel shell (~4500 lines across shell.c + shell_cmd_*.c + sh_script.c + shell_glob.c + shell_help.c) into a ring-3 ELF (`sh.elf`).  Multi-PR feature, NOT a single slice.  Slice 20a shipped as part of v0.8 (see slice 26); 20b–20f follow in subsequent rounds.  Each sub-PR is runnable in isolation; kernel shell stays the default until the final flip. | 🟡 in progress |
| 20a | `sh.elf` skeleton — read-parse-fork-execve-wait4 loop; builtins `cd` / `pwd` / `exit`.  Opt-in via `exec /apps/sh.elf` from any kernel shell prompt. | ✅ shipped (v0.8, see slice 26) |
| 20b | Inline editing + history — byte-by-byte input via `sys_getkey()` (KEY_ARROW_{UP,DOWN,LEFT,RIGHT} sentinels for cursor + history nav, backspace mid-line with tail-shift, Ctrl-C aborts line, Ctrl-D on empty line exits) + 16-entry ring-buffer history with dup-suppression.  Stays freestanding for `test_tcc_rebuild_sh`.  Test: `test_usershell_history`. | ✅ shipped |
| 20c | Variables + expansion: `NAME=value`, `$VAR` / `${VAR}` / `$?`, `env` / `unset` / `read` builtins.  Mirrors `sh_vars.c` + `sh_script.c`. | ⏭ |
| 20d | Control flow: `if` / `elif` / `else` / `fi`, `while`, `for ... in`, `[ TEST ]`, `true`/`false`/`sleep`.  Mirrors the rest of `sh_script.c`. | ⏭ |
| 20e | Tab complete + glob expansion.  Needs a streaming `SYS_READDIR` first (today's `SYS_LS_DIR` returns a pre-rendered text blob); that lands as its own micro-slice. | ⏭ |
| 20f | Boot wires `sh.elf` per VT (replaces the four in-kernel `shell0..shell3` tasks).  Kernel shell stays in-tree as a `/apps/sh.elf` fallback for boots where the ELF is missing.  **The boot-path commit; everything before it leaves the existing shell untouched.** | ⏭ |
| 27 | **Real rootfs + overlay mount layout** — replace slice 24's path-rewriting `resolve_rootfs_prefix` with a proper VFS mount table.  Rootfs (ext2 preferred, FAT32 acceptable) mounts at `/` and holds physical dirs `/usr /apps /src /docs /root /home`; FAT32 boot partition (limine + `makar.kernel`) mounts at `/boot`; overlays `/dev` (devfs), `/proc` (procfs), `/tmp` + `/log` (tmpfs, new), `/mnt` (bare dir for user mounts).  `/root` = root user's home dir.  No initrd/initramfs needed — kernel already has IDE + partition parsing in ring 0, so `kernel_main` mounts the rootfs partition directly (selected via `root=/dev/hdaN` cmdline).  Slices: (a) VFS mount table refactor in `vfs.c`; (b) ext2 read-only driver in `arch/i386/fs/ext2.c`; (c) boot-time rootfs mount + `root=` cmdline; (d) `/boot` remap; (e) tmpfs overlay for `/tmp` + `/log`; (f) ext2 write support; (g) path migration sweep across kernel + userspace + scripts + ui_test + CLAUDE.md.  **Slice 27g (path migration sweep) shipped — `/mnt/hd` slot retired in vfs.c (single-partition disks fold into `/mnt/root`); ui-tests, ktest, userspace, shell, man pages, and docs converted to `/apps`, `/usr`, `/mnt/<name>` Unix-style paths.  27a–27f still ⏭.** | 🟡 in progress |
| 21 | **COM2 serial-input mode for ui-test runner** — kernel reads stdin from COM2 (or COM1 with a flag) and pipes it into the focused shell's keyboard ring.  Eliminates the sendkey-drop flake class; ui_runner can `printf '...' > /dev/qemu-com2` instead of dripping 50 sendkeys at 100 ms each.  Linux `console=ttyS0` pattern. | ⏭ |

### Userspace / libc porting

The long-term goal is a self-hosting userspace. Prerequisites and approach:

1. **musl libc** (preferred over glibc or uClibc for size):
   - Needs: `mmap`/`munmap`, `brk`/`sbrk`, `read`/`write`/`open`/`close`/`stat`, `fork`/`exec`/`wait` (or at minimum `posix_spawn`), `getpid`, signals.
   - Current blocker: no `fork` - Makar has cooperative tasks, not POSIX processes. Either implement `fork` (requires COW page tables) or target a no-fork musl config (`musl` + `MUSL_NO_FORK` equivalent).
   - Recommended path: add `SYS_OPEN`, `SYS_CLOSE`, `SYS_READ` (file), `SYS_WRITE` (file), `SYS_STAT`, `SYS_LSEEK` first, then `SYS_BRK` (heap extension), then `SYS_MMAP` (anonymous), then attempt musl.

2. **uClibc-ng** (lighter than musl, targets embedded - no fork required for static linking):
   - Still needs the file-I/O syscall set above plus `SYS_GETPID`, `SYS_UNAME`.
   - Static-link userspace apps against uClibc-ng for a known-good libc without porting musl's threading.

3. **bash / dash**:
   - Requires a working libc (musl or uClibc), `fork`+`exec`, file descriptors (stdin/stdout/stderr as VFS fds), `tcgetattr`/`tcsetattr` (terminal), `getenv`/`setenv`, `opendir`/`readdir`.
   - **dash** (POSIX sh, ~150 KiB) is more tractable than bash (~1 MiB) as a first shell port.
   - Near-term stand-in: extend the existing Makar shell with more builtins (pipes, redirection, variables) rather than porting dash immediately.

4. **File-descriptor layer**:
   - ✅ Per-task fd table landed (slice 12): each task owns a `fd_table_t` with fds 0/1/2 pre-bound to keyboard/VGA/VGA+serial; SYS_OPEN allocates higher slots, kind-tagged (`FD_KIND_FILE`, etc.).
   - Still needed for a real POSIX layer: streaming file reads (drop the eager-buffer model), `dup`/`dup2`, and `pipe` (SYS_PIPE - sketchable once the table exists, since fd creation no longer has to round-trip through SYS_OPEN).

5. **Process model**:
   - True POSIX processes require `fork` (COW) + separate address spaces. The current VMM can map per-task page directories; `fork` would clone one.
   - Alternative: implement `posix_spawn` semantics (create + exec without fork) - sufficient for a non-interactive shell and simpler to implement.

### Hardware / platform
- **USB HID keyboard**: currently PS/2 only. QEMU emulates PS/2 by default; real hardware may need USB HID via OHCI/EHCI.
- **Network**: RTL8139 driver → lwIP → DHCP/DNS → wget/curl-lite.
- **64-bit (x86-64)**: significant rewrite - new GDT/IDT, long mode entry, 64-bit paging. Worth considering once userspace is stable on i386.

## Next: "Serious dev work in-place" (write, compile, run C on a live Makar system)

See `SURVEY.md` for complete inventory of shell commands, userspace apps, VFS/FAT32 APIs, and the installer.

### Kernel prerequisites (must land first)
1. **`SYS_WRITE(fd, buf, len)`** - fix EAX=4 to standard Linux i386 convention (fd + buffer + length). Unblocks all libc stdio.
2. **`SYS_GETCWD`** - ✅ shipped (215). `SYS_READDIR` still needed for streaming `ls` (the current `SYS_LS_DIR` returns a pre-rendered text blob).

### Libc / toolchain
3. **musl static link** - once the fd table and `SYS_BRK` exist, a musl static binary compiles with the existing i686-elf cross-compiler. See `docs/userland-libc.md` for the step-by-step.
4. **uClibc-ng** as a lighter fallback if musl proves difficult without `fork`.

### In-kernel compiler
5. **TCC (Tiny C Compiler)** - ~200 KiB, compiles C to ELF in memory, writes output via `vfs_write_file`. No `fork` needed. Enables write-compile-run on bare metal, CP/M-style.

### Networking (longer-term, same PR series)
6. **NIC driver** - RTL8139 is the primary target (well-documented, QEMU `-device rtl8139`). AMD PCNet (`-device pcnet`) is the QEMU default and also well-documented.
7. **lwIP** - BSD-licensed, small footprint, designed for embedded. Needs a `sys_arch` adapter and a packet Rx/Tx hook from the NIC driver.
8. **DHCP + DNS stubs** - lwIP includes both; just need the netif glue.
9. **wget/curl-lite** - a minimal HTTP GET over lwIP. No TLS initially; TLS via mbedTLS or BearSSL later.

### Process model (prerequisite for userland shell)
10. **`fork()` or `posix_spawn`** - COW page-table clone (or simpler: exec-without-fork via `posix_spawn` semantics). Required before moving the shell to userland. See `docs/userland-libc.md` roadmap graph.
