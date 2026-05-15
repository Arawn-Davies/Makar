# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Makar is a hobby x86 (i386) bare-metal OS kernel written in C and AT&T assembly, booted via GRUB Multiboot 2. It targets 32-bit protected mode and runs in QEMU. Docker wraps the full build/test toolchain - no host cross-compiler is required.

## Build commands

All build, test, and boot operations go through a single entrypoint:

```sh
# Day-to-day (build + run in one shot)
./run.sh iso-boot       # clean → debug ISO → interactive QEMU
./run.sh iso-test       # full CI suite: ktest + GDB boot-checkpoint tests
./run.sh iso-ktest-gui  # test ISO → ktest with display window (needs host QEMU)
./run.sh iso-release    # optimised release ISO

./run.sh hdd-boot       # clean → build kernel → HDD image → interactive QEMU
./run.sh hdd-test       # clean → build kernel → HDD image → GDB boot test
./run.sh hdd-release    # HDD image only

# CI-style split modes (build once, run many — used by .github/workflows/build-test.yml)
./run.sh iso-build      # kernel + makar.iso + makar-test.iso, no run
./run.sh hdd-build      # kernel + makar-hdd-test.img, no run
./run.sh ktest-run      # ktest against existing makar-test.iso
./run.sh gdb-iso-run    # GDB ISO boot test against existing makar.iso
./run.sh gdb-hdd-run    # GDB HDD boot test against existing makar-hdd-test.img

./run.sh clean          # remove all build artefacts

# Internal scripts (called inside the Docker container - do not invoke directly):
./build.sh              # compile kernel + libc (parallel via -j$(nproc), ccache-wrapped)
./iso.sh                # build + package: emits makar.iso always, makar-test.iso when TEST_ISO=1
./clean.sh              # remove build artefacts
./generate-hdd.sh       # create raw MBR + FAT32 HDD image with GRUB 2

# Docker Compose equivalents (prefer run.sh for day-to-day use):
docker compose run --rm build          # release ISO
docker compose run --rm build-debug    # debug ISO (-O0 -g3)
docker compose run --rm test           # full iso-test suite
```

Debug builds use `-O0 -g3`; release uses `-O2 -g`. Override via `CFLAGS`.

**Single-kernel, two-ISO model** (post PR #125): one kernel binary is built once; `iso.sh` packages `makar.iso` (interactive GRUB menu, default to live shell) and `makar-test.iso` (single menuentry, `timeout=0`, `multiboot2 /boot/makar.kernel test_mode`). The `test_mode` flag is still a runtime cmdline arg parsed from the multiboot2 CMDLINE tag — there is no compile-time test flag, and the ISO/HDD images share the exact same `makar.kernel`. `build.sh` uses `-j$(nproc)` and wraps `i686-elf-gcc` in `ccache` automatically (set `CCACHE=0` to disable).

**ccache toolchain image** (`makar-build:local`): `Dockerfile` layers `ccache` on top of `arawn780/gcc-cross-i686-elf:fast`, mounts `.ccache/` from the workspace, and is auto-built by `run.sh` on first use. Warm rebuilds are ~3× faster (16.9 s cold → 5.6 s warm, ~47 % cache hit rate). The upstream image is still used directly inside the GitHub Actions `container:` jobs (ktest, gdb-iso, gdb-hdd) where ccache is unnecessary because the build is consumed as an artifact.

**KVM acceleration** is gated behind `MAKAR_USE_KVM=1` (off by default). KVM was attempted for CI speedup but produced reproducible failures: software breakpoints under the GDB stub never catch, and the ktest path-fault was masked by KVM's CPU timing differing from TCG. Leave off unless you are explicitly debugging KVM compatibility.

`run.sh` execution context (checked in order):
1. `/.dockerenv` present (container / CI) → run steps directly
2. Docker CLI available → wrap in `docker run`
3. `i686-elf-gcc` on PATH (native tools) → run steps directly
4. None of the above → error with install hints

QEMU steps prefer host `qemu-system-i386` when Docker is the build context; fall back to the container. GDB test steps use host qemu + gdb-multiarch together if both present; otherwise run inside the container.

## Testing

**Full CI suite** (`iso-test`: ktest + GDB boot checkpoints):
```sh
./run.sh iso-test
# Phase 1: test_mode ISO → ktest_run_all() → QEMU exits.  Output: ktest.log
# Phase 2: debug ISO + FAT32 test disk → full GDB test suite.  Output: gdb-test.log
# exits 0 on pass, 1 on any failure
```

**HDD boot test:**
```sh
./run.sh hdd-test
# Builds kernel → generates makar-hdd-test.img → GDB boot test (no CD-ROM)
# outputs: hdd-test-gdb.log, hdd-test-serial.log
```

Both GDB test scripts (`tests/gdb_boot_test.py`, `tests/gdb_hdd_test.py`) run all four groups:

| Group | What it verifies |
|---|---|
| `boot_checkpoints` | Every major boot function reached (`kernel_main` → `shell_run`) |
| `hardware_state` | CR0.PG set, CR3 non-zero, PIT is ticking |
| `vesa` | VESA framebuffer / TTY init state |
| `hdd_mount` | `fat32_mounted()` non-zero - FAT32 auto-mounted at `/hd` |

The ISO GDB test attaches a 32 MiB FAT32 test disk (created via `mkfs.fat --offset`, no losetup needed) so `hdd_mount` is valid on the CD-ROM boot path too.

**Interactive GDB debug** (inside Docker container manually):
```sh
docker run --rm -it -v "$PWD:/work" -w /work arawn780/gcc-cross-i686-elf:fast \
    bash -c 'qemu-system-i386 -cdrom makar.iso -s -S -display none -serial stdio &
             gdb-multiarch src/kernel/makar.kernel -ex "target remote :1234"'
```

`generate-hdd.sh` uses `grub-mkimage` (not `grub-install`) to avoid the UUID-search failure that `grub-install` produces when probing loop devices inside Docker. The FAT32 partition receives the kernel at `/boot/makar.kernel` and userspace binaries from `isodir/apps/` at `/apps/`.

**In-kernel test suite (interactive)**: shell command `ktest` runs all suites from the kernel shell.
At boot (when `test_mode` is *not* in the cmdline), `ktest_bg_task` runs all suites silently in the background - only prints to VGA on failure; always writes `KTEST_BG: PASS/FAIL` to serial.

**TODO:** 

Startup ktests: On startup, before we start the shell task we need to run background ktests that test capabilities without affecting the loading screen output. 
Only once all ktests silently pass may the loading screen progress and we start the shell. 
Make sure it's a bit of a delay between each test so the startup screen is visible. Print to serial should be remain. 
- The spinner loop is inside ```if (vesa_tty_is_ready())``` - if VBE isn't active (VGA fallback), the whole block is skipped and we drop straight into the REPL.  
- The wait must be outside that conditional. 

## Architecture

### Boot sequence (`kernel_main`)
1. `terminal_initialize` → `init_serial(COM1)` → `init_descriptor_tables` (GDT+IDT)
2. Exception handlers, PMM, paging (256 MiB identity map, 4 MiB pages), heap
3. VESA init + display mode selection: 720p if Bochs VBE available, else 80×50 VGA text
4. Timer (100 Hz PIT), keyboard (layered PS/2 driver), IDE/VFS
5. `tasking_init` + `task_create("shell", shell_run)` + `task_create("ktest", ktest_bg_task)`
6. `syscall_init` (registers `int 0x80` handler at IDT gate DPL=3)
7. Idle loop: `task_yield` + `hlt`

### Display mode selection
At boot, `kernel_main` calls `bochs_vbe_available()`. If the Bochs VBE I/O ports respond (QEMU `-vga std`), it sets 1280×720×32 and initialises the VESA TTY at `font_scale=2` (40-col equivalent at this res). If VBE is absent (hardware or minimal QEMU config), it falls back to VGA 80×50 text mode.

The `setmode` shell command can switch freely between any supported resolution at runtime.

### Memory map
- `0x00000000–0x0FFFFFFF` (256 MiB): kernel identity window (4 MiB large pages)
- `0x40000000` (`USER_CODE_BASE`): ring-3 code page
- `0xBFFF0000` (`USER_STACK_TOP`): ring-3 stack top (one 4 KiB page below)

### Tasking
Round-robin scheduler with timer-driven preemption (PIT 100 Hz; IRQ 0 yields every `SCHED_QUANTUM=4` ticks ≈ 40 ms). Cooperative `task_yield()` is also available for explicit yields. Context switch via `task_asm.S` (callee-saved + EFLAGS). `task_exit()` marks the task DEAD and yields; the scheduler reaps the dead task's user page directory after switching CR3 away from it (`schedule()` reaper, `task.c`). Pool is fixed-size (`MAX_TASKS=8`).

Per-task state (`task_t` in `kernel/task.h`):
- `pid` - monotonically assigned (idle = 1, others from 2)
- `cwd[VFS_PATH_MAX]` - inherited from creator; not yet authoritative (VFS still uses `s_cwd`)
- `tty` - TTY index (TASK_TTY_NONE for unbound); not yet authoritative (vtty.c still uses `vtty_tasks[]`)
- `sig_pending` / `sig_mask`  Linux-style signal bitmasks (subsystem to follow)
- `fd_table` - opaque pointer (placeholder until per-task fd table lands)
- `user_brk`, `page_dir`, `state`, `name`, `esp`, `stack`, `next`

### Syscall ABI (`int 0x80`, Linux i386 convention)
Authoritative table in `src/kernel/include/kernel/syscall.h`. Selected entries:

| EAX | Syscall          | Args |
|-----|------------------|------|
| 1   | SYS_EXIT         | EBX = status |
| 3   | SYS_READ         | EBX = fd (0=stdin keyboard, ≥3=VFS), ECX = buf, EDX = count |
| 4   | SYS_WRITE        | EBX = fd, ECX = buf, EDX = count. fd 1 = VGA, fd 2 = VGA + COM1, ≥3 = VFS |
| 5   | SYS_OPEN         | EBX = path, ECX = flags (returns fd) |
| 6   | SYS_CLOSE        | EBX = fd |
| 19  | SYS_LSEEK        | EBX = fd, ECX = offset, EDX = whence |
| 45  | SYS_BRK          | EBX = new break (returns current/new break) |
| 100 | SYS_DEBUG        | EBX = uint32 checkpoint (prints to VGA + serial) |
| 158 | SYS_YIELD        | - |
| 200 | SYS_GETKEY       | raw single-char keyboard read |
| 201–204 | SYS_PUTCH_AT / SET_CURSOR / TTY_CLEAR / TERM_SIZE | direct TTY ops for full-screen apps (vix) |
| 205 | SYS_WRITE_FILE   | path, buf, len |
| 206 | SYS_LS_DIR       | path, buf, bufsz |
| 207 | SYS_DISK_INFO    | buf, bufsz |
| 208–210 | SYS_DELETE_FILE / RENAME_FILE / DELETE_DIR | FAT32 mutations |
| 211 | SYS_WRITE_SERIAL | buf, len — COM1-only (no framebuffer) |
| 212 | SYS_KEYBOARD_RAW | enable/disable raw mode (1 = raw bytes, no sentinel translation) |
| 213 | SYS_SHELL_CLEAR  | same as `clear` shell builtin |
| 214 | SYS_UPTIME       | returns 100 Hz PIT tick counter |

### Keyboard (layered driver, PR #124)
Stack: PS/2 IRQ → scancode (set-1 + 0xE0 prefix) → keycode (HID-style abstract code) → ASCII/sentinel → per-TTY ring → consumer (shell, kbtester).

- **Sentinels** (non-ASCII control codes) at `0x80`–`0x83` for arrow keys, `0x84`–`0x8F` for F-keys, `0x90`+ for modifier events. Every cast on the dispatch path must be `unsigned char` to avoid sign-extension hazards — see slice 5b in the roadmap for the regression caught by `kbtester.elf`.
- **IRQ-driven SPSC ring per task** (up to `KB_TASK_SLOTS=4`); `keyboard_getchar()` registers the caller and blocks-yields on its slot; `keyboard_poll()` is non-blocking.
- **Make/break separation** is strict; modifier state is held at the decoder layer, not by consumers. Caps Lock toggle currently lacks a typematic-repeat filter (slice 5b).
- **Ctrl+A** arms the pane-switch dispatcher (Ctrl-A,U / Ctrl-A,J).
- **Ctrl+C** sets `g_sigint=1` AND routes `\x03` to the focused task's ring. `keyboard_sigint_consume()` atomically reads and clears `g_sigint`.
- **LED sync** is unimplemented — kernel never writes `0xED <bitmap>` to the PS/2 controller and does not read physical LED state at boot (slice 5b).
- `kbtester.elf` is the live diagnostic — dumps every scancode/keycode/sentinel and the modifier state vector to serial.

### Shell features
- Inline editing (cursor movement, insert at point).
- History navigation (↑/↓ arrows), up to 16 entries.
- `!!` recalls and runs the most recent history entry (echoes the recalled line first so the operator sees what's about to run).
- Ctrl+C: abort current input line (prints `^C`, returns empty line to REPL).
- Tab completion: first token completes command names; subsequent tokens complete VFS paths via `vfs_complete()` → `fat32_complete()`.
- `exec <path>`: loads and runs an ELF binary from the VFS. Ctrl+C during exec force-kills the child task.

### VMM (per-task page directories)
- `vmm_create_pd()` - allocates a page directory and mirrors kernel PDEs (indices 0–63)
- `vmm_map_page(pd, vaddr, paddr, flags)` - installs 4 KiB mapping; creates page tables on demand with `PAGE_USER`
- `vmm_switch(pd)` - loads CR3

### Ring-3 entry (`ring3.S`)
`ring3_enter(entry, stack_top)` loads user data selector (0x23) into DS/ES/FS/GS, builds a 5-word `iret` frame (SS=0x23, ESP, EFLAGS|IF, CS=0x1B, EIP), and executes `iret`. Never returns. Caller must call `tss_set_kernel_stack()` and `vmm_switch(pd)` first.

### Userspace apps (`src/userspace/`)
Freestanding ELF binaries built with the cross-compiler. Link against `crt0.S` + `link.ld`. Loaded and executed by `elf_exec()` (shell `exec` command). Available apps:

| Binary | Description |
|--------|-------------|
| `hello.elf` | Hello-world smoke test |
| `calc.elf` | bc-style expression calculator - `+`, `-`, `*`, `/`, `%`, parentheses, recursive-descent parser |
| `echo.elf` | echo argv to stdout |
| `ls.elf` | directory listing via `SYS_LS_DIR` |
| `rm.elf` / `mv.elf` / `cp.elf` | FAT32 file ops (also available as shell builtins) |
| `diskinfo.elf` | partition table + FAT32 BPB dump via `SYS_DISK_INFO` |
| `vix.elf` | pane-aware vi-style text editor; uses `SYS_PUTCH_AT` / `SYS_SET_CURSOR` / `SYS_TERM_SIZE` |
| `kbtester.elf` | keyboard diagnostic — logs every event (scancode/keycode/sentinel/modifier) to serial via `SYS_WRITE_SERIAL` |
| `help.elf` | replaced by `lsman` / `man <cmd>` shell builtins; kept for compatibility |

### ktest harness
`KTEST_ASSERT(expr)` - records pass/fail to VGA + serial.
`KTEST_ASSERT_EQ(a, b)` - equality variant.
`KTEST_ASSERT_MAJOR(expr)` - like `KTEST_ASSERT` but calls `kpanic_at` on failure; use for invariants whose violation indicates kernel corruption (GDT validity, PMM sanity, etc.).

## Debug output
- VGA: `t_writestring`, `t_hex`, `t_dec`, `t_putchar` (`include/kernel/tty.h`)
- Serial: `Serial_WriteString`, `Serial_WriteHex` (`include/kernel/serial.h`)
- `KLOG` / `KLOG_HEX` macros - serial only, require `-DDEV_BUILD` compile flag (no-ops in release)
- `SYS_DEBUG` writes to **both** VGA and serial unconditionally - preferred for ring-3 debugging
- `kpanic(msg)` / `KPANIC(msg)` / `kpanic_at(msg, file, func, line)` - renders a panic screen and halts

## Key source layout
```
src/kernel/arch/i386/
  boot/       boot.S (Multiboot 2 entry), crti/crtn
  core/       GDT/IDT (descr_tbl.c), ISR stub (isr_asm.S), interrupt dispatch (isr.c)
  mm/         pmm.c (frame allocator), paging.c, vmm.c (per-task page dirs), heap.c
  drivers/    serial, keyboard, timer, IDE, ACPI, partition
  fs/         fat32.c, iso9660.c, procfs.c (synthetic /proc), vfs.c
  display/    tty.c (VGA text), vesa.c + vesa_tty.c (VESA framebuffer), vt.c (per-TTY backing grid)
  proc/       task.c + task_asm.S (scheduler), syscall.c, ring3.S, usertest.c, ktest.c, vtty.c
  shell/      shell.c, shell_cmd_{display,disk,fs,apps,system,man}.c, shell_help.c
  debug/      exception handlers (INT 1/3/8/13/14, serial-first output)
src/kernel/kernel/kernel.c   kernel_main
src/kernel/include/kernel/   all public headers
src/libc/                    minimal freestanding libc (string, stdio, stdlib) → libk.a
src/userspace/               freestanding ELF apps (calc.elf, hello.elf)
tests/                       GDB boot-test suite (gdb_boot_test.py) + test groups
```

## Acknowledgements and FOSS attribution

Makar draws on the work of many free and open-source projects.  All referenced
code is used in compliance with its licence; attribution is maintained in each
relevant source file and in `docs/userland-libc.md`.

| Project | Licence | Influence |
|---------|---------|-----------|
| **Linux kernel** | GPLv2 | Syscall ABI (i386 int 0x80), ELF loading model, process memory layout |
| **ELKS** | GPLv2 | Minimal libc / crt0 model; `vix` editor philosophy |
| **FUZIX** | GPLv2 | vi-style editor design; libc porting approach for small systems |
| **CP/M** | Historic | Terminal-owns-screen philosophy; self-contained program model |
| **musl libc** | MIT | Target libc for future userspace; syscall stub conventions |
| **lwIP** | BSD | Future TCP/IP stack candidate |
| **GRUB** | GPLv2 | Bootloader; Multiboot 2 tag format |
| **OSDev wiki** | CC-BY-SA | Cross-compiler setup, paging, descriptor table guidance |

## Current state (as of May 2026)

Makar boots to an interactive VESA shell with 4 independent TTYs.
Alt+F1–F4 switches between them; each is a separate **preemptive** kernel
task with its own kernel stack, its own backing screen buffer (so a
background TTY's accumulated output survives a focus switch — Linux VT
behaviour), and (for ring-3 programs) its own page directory. Major
subsystems:

- **Display**: VESA framebuffer (Bochs VBE, defaults to 720p), VGA text fallback (80×50). Pane abstraction (`vesa_pane_t`) used by VIX. Per-TTY logical character grid (`vt_buf_t` in `display/vt.c`) backs every shell — writes go to the grid first; the framebuffer is only painted when that TTY is focused.
- **Multi-TTY**: 4 shell tasks (`shell0`–`shell3`). `vtty.c` routes keyboard input via `task_t.tty` (authoritative) and tracks the focused slot. `vtty_switch()` defers the framebuffer repaint out of IRQ context to `vtty_drain_pending()`, which runs from the destination shell's `keyboard_getchar` poll loop. A tmux-style status bar lives in the reserved bottom row showing `Makar  VT0  VT1  VT2  VT3  ...  Alt+F1-F4` with the active slot highlighted.
- **VIX**: Pane-aware text editor. Derives column/row counts from the active `vesa_pane_t` at runtime - works correctly at any VESA resolution. Modelled on ELKS/FUZIX vi: lightweight, stable, no heap after startup.
- **Storage**: FAT32 (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. VFS layer with CWD, auto-mount. Full read/write/delete/rename support on FAT32. Synthetic `/proc` mount exposes `cpuinfo`, `meminfo`, `tasks`, `uname` as read-only files generated on demand.
- **Tasking**: Round-robin scheduler with timer-driven preemption (PIT 100 Hz, `SCHED_QUANTUM = 4` ticks → 40 ms slice). Per-task `pid`, `cwd`, `tty`, signal bitmasks, fd-table placeholder. User PD reaped on task exit. Background ktest harness runs before the shell prompt appears.
- **Userspace**: Ring-3 protected mode via `iret`. ELF loader (`elf_exec`) with argc/argv. Syscalls: `SYS_EXIT`, `SYS_READ`, `SYS_WRITE` (fd 1 = VGA, fd 2 = VGA + COM1 serial), `SYS_OPEN`, `SYS_CLOSE`, `SYS_LSEEK`, `SYS_BRK`, `SYS_DEBUG`, `SYS_YIELD`, plus Makar extensions (200–214 - terminal/file ops + `SYS_WRITE_SERIAL`). Apps: `calc.elf`, `hello.elf`, `ls.elf`, `echo.elf`, `vix.elf`, `diskinfo.elf`, `rm.elf`, `mv.elf`, `cp.elf`, `kbtester.elf`.
- **Shell**: Inline editing, history, tab completion, Ctrl+C sigint. `lsman` / `man <cmd>` replace `help`. Built-in file ops: `rm`, `rmdir`, `mv`. `uptime` shows humanised h/m/s. `cat /proc/<entry>` for system introspection.
- **GRUB**: Two-entry menu (Makar OS + Next available device), 5-second timeout.

## Recently merged

| PR | Branch | Summary |
|---|---|---|
| #120 | `feat/userspace-fileops` | FAT32 delete/rename APIs, VFS wrappers, syscalls 208–210, shell builtins `rm`/`rmdir`/`mv`, ELFs `rm.elf`/`mv.elf`/`cp.elf` |
| #123 | `feat/tty-multitasking` | Preemptive 100 Hz scheduler, per-task `task_t` plumbing (pid/cwd/tty/sig/fd), user-PD reaper, ring-3 lifecycle ktest, `SYS_WRITE_SERIAL`, humanised `uptime` |
| #124 | `feat/keyboard-layered` | Layered PS/2 driver rewrite (scancode → keycode → sentinel → router), IRQ-driven per-TTY rings, `kbtester.elf` ring-3 diagnostic |
| #125 | `feat/test-infra-cleanup` | ccache toolchain image, single-kernel/two-ISO emit, build-once fan-out CI (4 parallel jobs), KVM auto-detect (off by default), `act` local validation, new split `*-build`/`*-run` modes in `run.sh` |
| #127 | `feat/keyboard-hygiene` | Keyboard hardening - `unsigned char` audit complete, typematic-repeat filter for modifiers, PS/2 LED sync (`0xED <bitmap>`), boot-time LED state read |
| #128 | `fix/reaper-uaf` | Reaper UAF (deferred PD free), keyboard IRQ-init order fix, loading-bar progress on startup, isolate ring-3 lifecycle suites from bg ktest |

## Future roadmap

### Slice queue (`feat/tty-multitasking` → follow-ups)

Tracked here, pulled into branches one at a time so each PR stays focused.

| # | Slice | Status |
|---|---|---|
| 1 | **Reaper for dead-task user PDs** | ✅ shipped (`fcb8771`) |
| 2 | **Per-task `task_t` plumbing** (pid/cwd/tty/fds/signals fields, no consumer migration) | ✅ shipped (`3a0ef78`) |
| 3 | **Ring-3 lifecycle ktest** with serial proof | ✅ shipped (`f48d730`, `1a34c20`) |
| 4 | **100 Hz timer + humanised uptime** + stderr→serial + `SYS_WRITE_SERIAL` | ✅ shipped (`5e40001`) |
| 5 | **Keyboard rewrite** - full PS/2 set-1 + e0, layered decoder (scancode→keycode→ASCII/sentinel→router), IRQ-driven per-TTY rings with proper SPSC memory ordering, strict make/break separation, modifier state at decoder, key repeat / rollover / lost-IRQ recovery, `unsigned char` end-to-end (no sign-extension hazard for sentinel compares), escape-clean sentinels | ✅ shipped (#124) |
| 5b | **Keyboard hardening** - `unsigned char` audit, typematic-repeat filter for modifiers, PS/2 LED sync, boot-time LED state read | ✅ shipped (#127) |
| 6 | **Test-infra cleanup** - ccache, single-kernel/two-ISO, build-once fan-out CI, KVM gate | ✅ shipped (#125) |
| 7 | **Per-task consumer migration** - vtty `task->tty` authoritative (drop `vtty_tasks[]` parallel array). VFS `task->cwd` and real per-task FD table remain on the queue. | ✅ partial (vtty done) |
| 8 | **Linux-style signal subsystem** - full sigaction table, `kill()` syscall, htop-style picker | ⏭ |
| 9 | **Preemption hardening** - interrupt-safe `schedule()`, per-task tick accounting, runtime-tunable quantum, busy-loop ktest | ⏭ |
| 10 | **Per-TTY screen buffers** - `vt_buf_t` backing grid per TTY, write-through to FB only when focused, repaint on Alt+Fn (deferred out of IRQ). tmux-style status bar at bottom row. `/proc` synthetic FS with `cpuinfo/meminfo/tasks/uname`. VGA-text fallback path stays on shared buffer (deferred). | ✅ shipped (this PR) |
| 11 | **`ps`-style task listing** with privilege/state/CWD/TTY columns | ⏭ (covered by `cat /proc/tasks` for now) |
| 12 | **fork() readiness** - PD clone (CoW), fd dup, PID alloc, return-value split | ⏭ |
| 13 | **UTF-8 terminal** with ASCII fallback / runtime mode switch | ⏭ deferred |
| 14 | **Per-task FD table** - replace global keyboard owner + opaque `fd_table` placeholder with a real array; pipe(2) / dup(2) fall out naturally | 🔥 **NEXT** |
| 15 | **VFS `task->cwd` authoritative** - drop the `s_cwd` global in `vfs.c`; resolve relative paths against the calling task's cwd | ⏭ |
| 16 | **VGA-fallback per-TTY** - route `tty.c` writes through `vt_buf` so VGA-text mode gets the same per-TTY isolation that VESA already has | ⏭ |

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
   - Current `SYS_READ` only reads from the keyboard (fd 0). Needs a proper fd table mapping integers to VFS nodes/keyboard/serial.
   - Once fds exist, pipes (`SYS_PIPE`) become straightforward.

5. **Process model**:
   - True POSIX processes require `fork` (COW) + separate address spaces. The current VMM can map per-task page directories; `fork` would clone one.
   - Alternative: implement `posix_spawn` semantics (create + exec without fork) - sufficient for a non-interactive shell and simpler to implement.

### Hardware / platform
- **USB HID keyboard**: currently PS/2 only. QEMU emulates PS/2 by default; real hardware may need USB HID via OHCI/EHCI.
- **Network**: RTL8139 driver → lwIP → DHCP/DNS → wget/curl-lite. See Next PR section above.
- **64-bit (x86-64)**: significant rewrite - new GDT/IDT, long mode entry, 64-bit paging. Worth considering once userspace is stable on i386.

## Documentation

- `docs/userland-libc.md` - how to build and link a freestanding libc for Makar userspace; step-by-step from syscall fixes through musl/uClibc-ng to TCC in-kernel compilation. Includes FOSS attribution and OSDev wiki references.

---

## Next: "Serious dev work in-place" (write, compile, run C on a live Makar system)

See `SURVEY.md` for complete inventory of shell commands, userspace apps, VFS/FAT32 APIs, and the installer.

### Kernel prerequisites (must land first)
1. **`SYS_WRITE(fd, buf, len)`** - fix EAX=4 to standard Linux i386 convention (fd + buffer + length). Unblocks all libc stdio.
2. **`SYS_GETCWD` + `SYS_READDIR`** - needed for shell tab completion and `ls` from userspace.

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
