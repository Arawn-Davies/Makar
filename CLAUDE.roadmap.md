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
| 14 | **makbox multicall + `SYS_GETCWD` + exec race fix** — busybox-style `makbox.elf` (ls/cat/cp/mv/rm/rmdir/echo/pwd); `SYS_GETCWD(215)`; per-task `exec_params` (closes the cross-TTY `CS=0x3F8` exec race); in-guest tests refactored to shared-VM runner. | ✅ |
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
| 25 | **In-OS TCC self-host milestone (v0.8 headline)** — `tcc.elf` ships on every ISO; `tcc /src/userspace/calc.c -o /tmp/calc.elf` and `tcc /src/userspace/sh.c -o /tmp/sh.elf` rebuild correct binaries in-OS.  Fixed upstream NULL-deref in `vendor/tinycc/tccelf.c:fill_local_got_entries`.  `SYSCALL_FILE_MAX` 8 → 16 MiB; kernel heap 16 → 32 MiB.  Tests: `test_tcc_hello`, `test_tcc_hello_relpath`, `test_tcc_rebuild_calc`, `test_tcc_rebuild_sh` + (post-v0.8) `test_tcc_rebuild_hello` + `test_tcc_rebuild_makbox` covering the other freestanding apps. | ✅ shipped (v0.8 + post) |
| 31 | **Kernel self-host (v0.9 headline)** — `./build-kernel-tcc.sh` (host) rebuilds a valid Multiboot 2 kernel ELF from the vendored source tree with only our shipped TCC; QEMU boots it through full subsystem init.  Generates `src/userspace/rebuild-kernel.sh` from the same file list, staged at `/apps/rebuild-kernel.sh`, runs in-OS via `TEST_CMDLINE='test_mode test=rebuild-kernel'` → `REBUILD-KERNEL: ALL PASS` / `FAIL` marker.  Required: (a) `kmalloc` 4-byte size alignment (root cause of a long-tail heap corruption under heavy fork/exec — without this, any odd-sized allocation misaligned the next remainder block and the freelist poisoned itself over many cycles); (b) NULL-guard in `vendor/tinycc/tccasm.c:asm_expr_sum` for forward-ref subtractions; (c) four `__TINYC__`-gated source tweaks (`boot.S` header-in-`.text`, `chainload.S` far-jmp offset hardcode, `isr_asm.S` GAS-`.macro`-to-cpp-`#define` rewrite, `vtty.c`+`keyboard.c` `kernel/atomic.h` shim); (d) hoist of `SYS_READDIR`'s nested callback to file scope; (e) drop `__builtin_unreachable()` under TCC in `libc/stdlib/abort.c`; (f) ISO staging of `/usr/include/kernel-build/`, `/usr/lib/tcc/include/{stdint,limits}.h`, `/apps/kend.S`; (g) `feat(test)` opt-in `REBUILD-KERNEL` test_mode phase via `TEST_WANT_EXPLICIT`.  Boot banner now prints `Self-hosted kernel! (TCC build)` vs `Host-built kernel (GCC build)` based on `__TINYC__`.  Host build proven end-to-end; in-OS rebuild runs the full script with correct PASS/FAIL marker discipline but ~19/75 per-file compiles still fail due to a separate kernel-sh `exec` argv-passing bug (task #16; not a TCC issue, same `tcc.elf` works fine from an interactive shell).  See `docs/handoff-self-hosting.md`. | ✅ host shipped, in-OS in progress (v0.9) |
| 26 | **Ring-3 shell MVP** — `/apps/sh.elf` is a freestanding ring-3 shell (only `#include "syscall.h"`) with prompt, line input, tokenize, `cd`/`pwd`/`exit` builtins, fork/execve/wait4 dispatch.  SYS_CHDIR(12) added.  SYS_EXECVE auto-transfers keyboard focus + VT foreground to the new image; SYS_WAIT4 reverses the transfer on child reap.  Coexists with the in-kernel shell — opt-in via `exec /apps/sh.elf`.  Tests: `test_usershell_smoke`, `test_usershell_execve`, `test_tcc_rebuild_sh`. | ✅ shipped (v0.8) |
| 30 | **POSIX cheap-fillers** — close the gap between Makar's existing kernel surface and what POSIX names.  30a `SYS_GETPID(20)` / `SYS_GETPPID(64)` over existing `task_t.pid` / `parent_pid`; 30b POSIX-numbered `SYS_UNLINK(10)`/`SYS_RENAME(38)`/`SYS_MKDIR(39)`/`SYS_RMDIR(40)` aliasing the existing 208/209/210 handlers + new `vfs_mkdir` dispatch; 30c POSIX `opendir`/`readdir`/`closedir` libc (`dirent.c`) over `SYS_READDIR(141)` with a proper `DIR *`; 30d CMOS RTC factored into `arch/i386/drivers/rtc.c` + `SYS_GETTIMEOFDAY(78)` + `SYS_CLOCK_GETTIME(265)` (REALTIME + MONOTONIC); 30e `access()` over `stat`; 30f `errno.h` + errno threading on every 30a–30e wrapper.  Tests: `test_getpid`, `test_posix_fs_syscalls`, `test_rtc_unix_time`. | ✅ |

#### Open

| # | Slice | Status |
|---|---|---|
| 17 | **UTF-8 terminal** with ASCII fallback / runtime mode switch | ⏭ deferred |
| 18 | **`ps`-style task listing** with privilege/state/CWD/TTY columns — shipped as the `ps` shell command (`cmd_ps` in `shell_cmd_system.c`): PID/PPID/state/RING/TTY/NAME columns.  `tasks` + `cat /proc/tasks` remain the simpler views. | ✅ shipped |
| 19 | **VGA-fallback per-TTY** — route `tty.c` writes through `vt_buf` so VGA-text mode gets the same per-TTY isolation that VESA already has | ⏭ |
| 20 | **Userland shell (full parity, multi-PR feature)** — lift the in-kernel shell (~4500 lines across shell.c + shell_cmd_*.c + sh_script.c + shell_glob.c + shell_help.c) into a ring-3 ELF (`sh.elf`).  Multi-PR feature, NOT a single slice.  Slice 20a shipped as part of v0.8 (see slice 26); 20b–20f follow in subsequent rounds.  Each sub-PR is runnable in isolation; kernel shell stays the default until the final flip. | 🟡 in progress |
| 20a | `sh.elf` skeleton — read-parse-fork-execve-wait4 loop; builtins `cd` / `pwd` / `exit`.  Opt-in via `exec /apps/sh.elf` from any kernel shell prompt. | ✅ shipped (v0.8, see slice 26) |
| 20b | Inline editing + history — byte-by-byte input via `sys_getkey()` (KEY_ARROW_{UP,DOWN,LEFT,RIGHT} sentinels for cursor + history nav, backspace mid-line with tail-shift, Ctrl-C aborts line, Ctrl-D on empty line exits) + 16-entry ring-buffer history with dup-suppression.  Stays freestanding for `test_tcc_rebuild_sh`.  Test: `test_usershell_history`. | ✅ shipped |
| 20c | Variables + expansion: standalone `NAME=value` (RHS shell-expanded), `$VAR` / `${VAR}` / `$?` substitution across the whole line pre-tokenize, `env` / `unset` / `read` builtins.  Per-shell fixed-size table (32 slots × `VAR_VAL_MAX=192`).  Stays freestanding (still TCC-rebuildable).  Test: `test_usershell_vars`.  Quoting / inline `FOO=bar CMD` env-prefixes deferred to 20d. | ✅ shipped |
| 20d | Control flow: `if` / `elif` / `else` / `fi`, `while`, `for ... in`, `[ TEST ]`, `true`/`false`/`sleep`.  Mirrors the rest of `sh_script.c`.  Implemented in `sh.c` (`run_block_until` / `run_script_buf`; REPL routes `if`/`while`/`for` lines through the script interpreter so single-line `;`-forms run at the prompt).  Test: `test_usershell_control_flow`. | ✅ shipped |
| 20g (PR #181) | **POSIX shell A1-A3** — pipes (`cmd1 \| cmd2`), redirection (`<`, `>`, `>>`, `2>`, `2>>`), list operators (`&&`, `\|\|`, `&`) + 16-slot background `jobs[]` table + `wait` builtin.  All landed in `sh.elf` (userspace only — kernel sh interpreter is unchanged).  Pipelines fork each stage and `dup2` the shared `pipe_ring_t` (4 KiB); redirects extract from argv before dispatch; list ops gate segments on `$?`.  Slice 0 of the same PR also fixed an inline `if;then;else;fi` parser bug in the kernel `sh_script.c`. | ✅ shipped |
| 20e | Tab complete + glob expansion.  Needs a streaming `SYS_READDIR` first (today's `SYS_LS_DIR` returns a pre-rendered text blob); that lands as its own micro-slice. | ⏭ |
| 20f | Boot wires `sh.elf` per VT (replaces the four in-kernel `shell0..shell3` tasks).  Kernel shell stays in-tree as a `/apps/sh.elf` fallback for boots where the ELF is missing.  **The boot-path commit; everything before it leaves the existing shell untouched.** | ⏭ |
| 27 | **Real rootfs + overlay mount layout** — replace the path-rewriting `resolve_rootfs_prefix` hack with a real VFS mount table.  Rootfs mounts at `/`; `/dev` `/proc` `/tmp` `/log` `/boot` `/mnt/<name>` are all first-class table entries.  `root=/dev/hdaN` Multiboot2 cmdline selects rootfs explicitly; default auto-detect (ext2 → FAT32 → CD-ROM emergency).  `/root` mkdir'd best-effort on writable rootfs boots.  **All sub-slices shipped:** (a) mount-table refactor in `vfs.c` — longest-prefix-match against `s_mounts[]`, deleted `resolve_rootfs_prefix`/`resolve_usr_prefix`/`mount_is_elevated`/`cdrom_is_elevated`/`s_usr_prefix`/`s_rootfs_*`/`s_bootfs_*`/`hd_mount_t`/`hd_find*`; (b) ext2 driver pre-existed (1272 lines); (c) `vfs_mount_root(spec)` + `root=` parser in `kernel.c`; (d) `/boot` mirror entry added by `vfs_auto_mount` when FAT32 boot partition binds; (e) `/tmp` + `/log` overlays registered in `vfs_init`; (f) ext2 write support pre-existed; (g) consumer sweep already shipped earlier in this PR. | ✅ shipped |
| 21 | **~~COM2 serial-input mode for the test runner~~** — superseded.  The in-guest key-injection harness (`keyboard_inject_text`/`keyboard_inject_key` + `keyboard_test_driver`, `./run.sh kbtest`) already feeds keys straight into the live decode→ring→shell pipeline with no host typing and no flake class, so a serial-input bridge is unnecessary. | ✅ superseded by in-guest key injection |
| 28 | **POSIX pipes + dup/dup2 + streaming fd refactor** — kernel-side primitives that the userland shell needs for `|`, `>`, `<`, `2>&1`.  Sub-slices: (a) `pipe_ring_t` shared refcounted backing object in `fd.h` so `fd_table_clone` (fork) bumps refcounts instead of deep-copying — shipped for PIPE kind in PR #181; FILE kind still deep-copies (open_file_t refactor still pending); (b) `SYS_DUP2` (EAX=63) shipped in PR #181 with standard Linux i386 semantics (`dup2(oldfd, newfd)` closes newfd first); single-arg `SYS_DUP` still pending; (c) `SYS_PIPE` (EAX=42) + `FD_KIND_PIPE` + 4 KiB ring-buffered pipe, `task_yield()` block on empty/full, EOF when refcount_w hits 0, `-EPIPE` when refcount_r hits 0 — shipped in PR #181; (d) `SYS_READDIR` ring-3 hookup -- shipped (`sys_readdir` at `src/userspace/syscall.h`, consumed by `alloctest.c`).  (e) single-arg `SYS_DUP` (EAX=41) -- shipped: allocates the lowest free fd via `fd_alloc`, shallow-copies the slot, bumps `FD_KIND_PIPE` refcounts like `dup2`; `sys_dup` wrapper in `src/userspace/syscall.h`, `dup()` in `<unistd.h>`. | 🟢 mostly shipped: pipes + dup2 + dup + readdir done; FILE-kind open_file_t refcount still pending |
| 29 | **~~Host-typing shift-letter drop~~** — obsolete.  This was a host-input artifact (host-typed `shift-<letter>` dropping the letter), not a kernel bug: the in-guest key-injection harness builds the make/break pair itself (`char_to_kc` maps uppercase to keycode+shift) so uppercase identifiers inject correctly.  Tests no longer depend on host typing, so the workaround (lowercase-only) is moot. | ✅ obsolete (no host typing) |
| 33 | **Modern-hardware track (UEFI + USB HID + AHCI)** — make Makar bootable/usable on legacy-free UEFI machines.  Three independent sub-tracks: (a) **UEFI boot** — deferred; the planned path is the **Limine** bootloader (BIOS+UEFI native), which will **replace GRUB** in the distant future (vendor/limine already staged for the HDD installer).  NOT GRUB-EFI.  The kernel already consumes the MB2 framebuffer tag, but the real-mode chainload trampoline + BDA 0x417 probe break post-ExitBootServices and need addressing for legacy-free UEFI. (b) **USB HID** — `usb/usb.c` now detects controllers (UHCI/OHCI/EHCI/xHCI via PCI); next: a HCI driver + enumeration + HID **boot-protocol** kbd/mouse (fixed reports, no descriptor parse).  UHCI is QEMU-simplest; modern HW is xHCI-only.  Route HID input into the existing keycode/mouse rings so the WM + shell are unchanged. (c) **AHCI** — SATA driver (PCI class 0x01/0x06) to replace the legacy IDE controller on UEFI boxes that drop it.  (The legacy IDE path itself now does bus-master DMA, PR #190; AHCI is still a separate driver.)  Each is a large multi-slice subsystem. | 📋 planned (USB detect done) |
| 32 | **GUI window manager** — Cosmos-style double-buffered desktop.  **PoC shipped:** userspace `gui.elf` (type `gui`), PS/2 mouse driver (IRQ12, `SYS_MOUSE_READ`, `mouse_inject_packet`), `SYS_FB_PRESENT` compositor, draggable xterm-style terminal window, dock with live stat pills, desktop icons (Terminal/Files/Editor/Doom; Editor/Doom `execve` fullscreen), `SYS_KEYBOARD_RAW(2)` scancode passthrough, WM ignores SIGINT (Ctrl-C closes window not gui), status bar forced off in gui.  **Deferred:** Alt+F5/F6 VT switch (gui↔shell fb/kb/mouse focus handoff), live `sh.elf` in-window (needs non-blocking pipes), file-browser + windowed editor apps.  Design + queue in **`docs/plans/gui-wm.md`**. | 🟢 PoC shipped (`feature/gui_planning`) |
| 34 | **Doom port (userspace)** — vendored `ozkl/doomgeneric` at `vendor/doomgeneric`; ring-3 `doom.elf` via `doomgeneric_makar.c` (frames→`SYS_FB_PRESENT` 640×400 letterboxed, input→`SYS_KEYBOARD_RAW(2)` make/break, timing→`SYS_UPTIME`).  No sound (`FEATURE_SOUND` off; sound backends excluded).  libc fillers: `strcasecmp`/`strncasecmp`, `abs`/`atof`/`fabs`, `puts`/`putchar`; new userland headers `stdint/stddef/stdbool/strings/inttypes/limits/assert/math/fcntl/sys/{types,stat}.h`.  **Compiles + links** (1.7 MB).  WAD via `getwad.sh` from a plain-HTTP mirror (Makar wget is TLS-less; doomworld enforces https → not fetchable in-OS).  Remaining: in-guest run/tune, in-OS tcc build. | 🟢 builds (`feature/gui_planning`) |

### Userspace / libc porting

**Hosted-libc fillers (shipped):** the in-tree `libc.a` grew the hosted surface that ordinary
(non-freestanding) programs expect — `<unistd.h>` (`dup`/`dup2`/`pipe`/`sleep`/`usleep`/`_exit`
+ prototypes for the tcc_compat-owned `read`/`write`/`close`/`lseek`/`access`/…), writable
`environ` (`setenv`/`getenv`/`unsetenv`/`putenv`, process-local — does not cross `execve`),
`<string.h>` `strtok`/`strtok_r`/`strerror`, `<stdlib.h>` `bsearch` + `system()` (runs
`/apps/sh.elf -c`), and a real `<time.h>` (`struct tm`, `gmtime`/`localtime`/`mktime`/`strftime`,
integer-only, UTC).  `sh.elf` gained a `-c "<cmd>"` mode (backs `system`).  All verified by the
extended `alloctest` sub-tests #13–18 *and* the in-OS TCC self-rebuild of alloctest against the
new headers (`LIBC-TCC: [PASS] compile-alloctest`).  `sh.c` stays freestanding (`syscall.h`
only) so `test_tcc_rebuild_sh` is unaffected.

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

### PCI / PCIe bus (groundplane — in progress)

Skeleton landed (`src/kernel/arch/i386/drivers/pci.c` + `include/kernel/pci.h`).  `pci_init()` scans all 256 buses at boot via legacy I/O ports 0xCF8/0xCFC.  `lspci` shell command prints the full device table.

Remaining slices:
1. **ACPI MCFG parsing** — augment `acpi.c` to locate the PCIe MMIO config space base address (needed for extended config space > offset 0xFF and PCIe-only devices).
2. **Driver binding** — `pci_driver_t` registration table; `pci_probe_all()` walks `pci_devices[]` matching vendor/device or class/subclass to a registered driver.
3. **RTL8139 NIC** — `src/kernel/arch/i386/drivers/net/rtl8139.c`; PCI 10EC:8139.  BAR0 = I/O base.  Tx ring + Rx ring, IRQ handler, expose as `net0` device node.  Enables `ping`, `wget`, NFS mount.
4. **virtio-net** — for QEMU `-device virtio-net-pci`; simpler than real NIC (no undocumented register quirks).  Good companion driver to RTL8139.
5. **USB host controller detection** — `src/kernel/arch/i386/drivers/usb/usb.c`; detect UHCI/OHCI/EHCI/xHCI controllers via PCI class 0x0C:0x03.  Foundation only — full HID stack comes later.
6. **USB HID keyboard** — OHCI/UHCI driver + USB HID class + boot protocol keyboard; fallback when PS/2 is absent on real hardware.

### User accounts / login (groundplane — skeleton only)

Files: `src/kernel/arch/i386/auth/` (`sha256.c`, `shadow.c`, `login.c`, `auth.h`).  Not wired in yet.

Plan:
1. **SHA-256** — port from Medli `SHA256.cs` (261 lines C#) to freestanding C.  No salt yet, just `sha256_hex(input, out[65])`.
2. **`/etc/shadow` + `/etc/passwd`** — subset of Linux format.  `shadow_verify(user, pass)` reads `/etc/shadow`, extracts `$6$<salt>$<hash>`, re-hashes input with salt, compares.  `shadow_set_password` generates 16-char salt from PIT ticks + RTC entropy.
3. **Login screen** — `login_screen()` in `login.c`: full-screen white-on-blue prompt; masked password input; max 3 attempts.  Triggered when `sh.elf --login` receives `exit`/Ctrl-D (replace the current "cannot exit" message).
4. **`task_t` uid/gid fields** — add `uint16_t uid, gid` to `task_t`; populate on login; `task_is_admin()` checks `uid == 0`.
5. **`sudo` builtin** — in `sh.elf`: `sudo <cmd>` prompts for password, re-checks `shadow_verify`, forks child with uid=0.  Kernel side: `SYS_SETUID(23)` restricted to uid=0 or shadow-verified callers.
6. **`/etc/passwd` + `/home/<user>`** — `vfs_ensure_user_dirs()` creates standard home dirs; `adduser`/`deluser` shell commands.

### UEFI / modern platform support

**Current state:** Makar boots via GRUB 2 Multiboot 2, which acts as a bridge. GRUB itself is UEFI-aware (boots from an ESP, uses GOP for early output), then delivers Makar the same Multiboot 2 info structure regardless of whether the firmware is legacy BIOS or UEFI. This means Makar already boots on UEFI machines today — it just doesn't know or care; GRUB absorbs the difference.

**Short-term (no kernel changes, QEMU testing):**
- Test with `OVMF` UEFI firmware: `qemu-system-i386 -bios /usr/share/OVMF/OVMF_CODE.fd` — validate framebuffer, ACPI, PCI enumeration all work the same.
- Test with `-machine q35` (modern PCIe chipset model) vs. the default `-machine pc` (i440FX, classic ISA/PCI). `q35` exposes PCIe root ports and an ICH9 southbridge instead of PIIX3 — `lspci` output will differ and the SATA/AHCI path matters for disk access.

**Medium-term (kernel-visible UEFI differences):**
1. **GOP framebuffer** — on UEFI/OVMF, Bochs VBE I/O ports (`0x01CE`/`0x01CF`) may be absent; the framebuffer is a GOP linear framebuffer whose address is in the Multiboot 2 framebuffer tag. As of PR #190 the kernel **already handles the no-DISPI case**: when `bochs_vbe_available()` is false but the bootloader supplied an LFB, `kernel_main` adopts that linear framebuffer and brings `vesa_tty` up on it (this also fixed the Hyper-V Gen 1 black screen). What remains is runtime mode-setting: `setmode` still can't switch resolutions without DISPI — proper fix is GRUB's `videoinfo`/`set gfxmode` or a virtio-GPU device. (The FB is also mapped write-combining via PAT so it's usable-fast on real hardware.)
2. **ACPI RSDP on UEFI** — firmware places the RSDP in EFI config tables rather than the EBDA/BIOS ROM scan range. GRUB copies the RSDP pointer into the Multiboot 2 ACPI tag. `acpi.c` currently scans EBDA/ROM; add a fast-path that reads the MB2 ACPI v1/v2 tag first (OSDev: tag type 14/15), falls back to memory scan only on BIOS boots. This also surfaces the **MCFG** table needed for PCIe extended config space.
3. **q35 / ICH9 differences** — q35 uses an AHCI SATA controller (PCI class 01:06, prog_if 01) rather than legacy IDE. `ide.c` speaks to the legacy 0x1F0/0x170 I/O ports which won't exist on q35. Need an AHCI driver (or fall back to the CD-ROM path for live boots). Disk writes only matter for HDD install flows. (Note: `ide.c` now does bus-master DMA on the legacy PIIX controller — PR #190 — but that's the IDE/PIIX path, not AHCI; q35 still needs a separate AHCI driver.)

**Long-term — native x86-64 + UEFI:**
- A 32-bit kernel can boot via UEFI with a 32-bit UEFI firmware (IA-32 UEFI), but these are rare; virtually all modern UEFI firmware is 64-bit.
- The correct long-term path is a 64-bit kernel: new GDT (long-mode segments), IDT (64-bit gates), paging (4-level PT), SysCall/SysRet ABI, UEFI runtime services for time/NVRAM.
- A 64-bit Makar would also run natively in the majority of hosted environments (VirtualBox, VMware, Hyper-V, real hardware) without needing GRUB to bridge the bitness gap.
- **Recommended approach:** keep the i386 build working (it's the development sandbox); start a parallel `arch/x86_64/` subtree once the i386 userspace and driver stack is reasonably complete. The shell, VFS, and userspace ELFs are architecture-independent and would port with minimal changes.

**Modular architecture note:** the driver and subsystem split (`drivers/`, `fs/`, `mm/`, `proc/`, `display/`, `auth/`) already follows a module-per-directory pattern. Extending to x86-64 means a new `arch/x86_64/` alongside `arch/i386/`; shared kernel code (VFS, task scheduler logic, shell, crypto) lives under `kernel/` and is compiled once for whichever arch is targeted. The `make.config` per-arch object list is the seam — each arch provides its own `make.config` naming its objects, and the top-level Makefile picks the right one via `ARCH`.

### Hardware / platform
- **USB HID keyboard**: currently PS/2 only. QEMU emulates PS/2 by default; real hardware may need USB HID via OHCI/EHCI.
- **Network**: RTL8139 driver → lwIP → DHCP/DNS → wget/curl-lite.
- **64-bit (x86-64)**: significant rewrite - new GDT/IDT, long mode entry, 64-bit paging. Worth considering once userspace is stable on i386.
- **Host shared folder** (QEMU-assisted file exchange between guest and host directory): four options in increasing complexity:
  1. **Second FAT32 virtio-blk image** — `qemu-img create -f raw shared.img 64M`, format FAT32, pass as `-drive file=shared.img,if=virtio`. Host loop-mounts it to read/write files; guest already has FAT32 read/write. Coarse-grained (can't both hold open at once) but zero new driver work. Best near-term option.
  2. **Serial file transfer (COM2)** — bespoke length-prefixed framing over a second `-serial` channel; a small host-side daemon maps packets to a real directory. No new QEMU flags; works with the existing serial driver. COM1 stays for debug/ktest output.
  3. **TFTP over `-netdev user`** — QEMU's user-mode network exposes a host directory via TFTP root (`-tftp /path`). Guest downloads files with a trivial UDP client. Write-back requires a separate PUT channel or scp workaround.
  4. **VirtFS / 9P** — `-fsdev local,path=...,security_model=mapped -device virtio-9p-pci`. Most capable (bidirectional, live). Requires virtio-pci enumeration + a 9P filesystem driver in Makar — significant but well-documented (Linux 9p driver is the reference). Correct long-term path once virtio-pci exists.

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
