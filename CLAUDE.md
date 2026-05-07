# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Makar is a hobby x86 (i386) bare-metal OS kernel written in C and AT&T assembly, booted via GRUB Multiboot 2. It targets 32-bit protected mode and runs in QEMU. Docker wraps the full build/test toolchain — no host cross-compiler is required.

## Build commands

All build, test, and boot operations go through a single entrypoint:

```sh
./run.sh iso-boot       # clean → debug ISO → interactive QEMU
./run.sh iso-test       # full CI suite: ktest + GDB boot-checkpoint tests
./run.sh iso-ktest-gui  # TEST_MODE ISO → ktest with display window (needs host QEMU)
./run.sh iso-release    # optimised release ISO

./run.sh hdd-boot       # clean → build kernel → HDD image → interactive QEMU
./run.sh hdd-test       # clean → build kernel → HDD image → GDB boot test
./run.sh hdd-release    # HDD image only

./run.sh clean          # remove all build artefacts

# Internal scripts (called inside the Docker container — do not invoke directly):
./build.sh              # compile kernel + libc (parallel via -j$(nproc))
./iso.sh                # build + package into makar.iso
./clean.sh              # remove build artefacts
./generate-hdd.sh       # create raw MBR + FAT32 HDD image with GRUB 2

# Docker Compose equivalents (prefer run.sh for day-to-day use):
docker compose run --rm build          # release ISO
docker compose run --rm build-debug    # debug ISO (-O0 -g3)
docker compose run --rm test           # full iso-test suite
```

Debug builds use `-O0 -g3`; release uses `-O2 -g`. Override via `CFLAGS`.
Test mode uses `-DTEST_MODE`: kernel runs `ktest_run_all()` then exits QEMU via `isa-debug-exit`.
`build.sh` uses `-j$(nproc)` for parallel compilation automatically.

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
# Phase 1: TEST_MODE ISO → ktest_run_all() → QEMU exits.  Output: ktest.log
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
| `hdd_mount` | `fat32_mounted()` non-zero — FAT32 auto-mounted at `/hd` |

The ISO GDB test attaches a 32 MiB FAT32 test disk (created via `mkfs.fat --offset`, no losetup needed) so `hdd_mount` is valid on the CD-ROM boot path too.

**Interactive GDB debug** (inside Docker container manually):
```sh
docker run --rm -it -v "$PWD:/work" -w /work arawn780/gcc-cross-i686-elf:fast \
    bash -c 'qemu-system-i386 -cdrom makar.iso -s -S -display none -serial stdio &
             gdb-multiarch src/kernel/makar.kernel -ex "target remote :1234"'
```

`generate-hdd.sh` uses `grub-mkimage` (not `grub-install`) to avoid the UUID-search failure that `grub-install` produces when probing loop devices inside Docker. The FAT32 partition receives the kernel at `/boot/makar.kernel` and userspace binaries from `isodir/apps/` at `/apps/`.

**In-kernel test suite (interactive)**: shell command `ktest` runs all suites from the kernel shell.
At boot (non-TEST_MODE), `ktest_bg_task` runs all suites silently in the background — only prints to VGA on failure; always writes `KTEST_BG: PASS/FAIL` to serial.

**TODO:** 

HDD test/interactive should use the same kernel binary — mode controlled by a GRUB kernel argument rather than a separate `-DTEST_MODE` build.
- `kernel.c` needs to parse `MULTIBOOT2_TAG_TYPE_CMDLINE` (type 1) and set a runtime `test_mode` flag, replacing `#ifdef TEST_MODE` guards with `if (test_mode)`.
- `multiboot.h` needs `#define MULTIBOOT2_TAG_TYPE_CMDLINE 1`.

Startup ktests: On startup, before we start the shell task we need to run background ktests that test capabilities without affecting the loading screen output. 
Only once all ktests silently pass may the loading screen progress and we start the shell. 
Make sure it's a bit of a delay between each test so the startup screen is visible. Print to serial should be remain. 
- The spinner loop is inside ```if (vesa_tty_is_ready())``` — if VBE isn't active (VGA fallback), the whole block is skipped and we drop straight into the REPL.  
- The wait must be outside that conditional. 

## Architecture

### Boot sequence (`kernel_main`)
1. `terminal_initialize` → `init_serial(COM1)` → `init_descriptor_tables` (GDT+IDT)
2. Exception handlers, PMM, paging (256 MiB identity map, 4 MiB pages), heap
3. VESA init + display mode selection: 720p if Bochs VBE available, else 80×50 VGA text
4. Timer (50 Hz PIT), keyboard, IDE/VFS
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
Round-robin scheduler with timer-driven preemption (PIT IRQ 0 yields every `SCHED_QUANTUM=4` ticks ≈ 80 ms at 50 Hz). Cooperative `task_yield()` is also available for explicit yields. Context switch via `task_asm.S` (callee-saved + EFLAGS). `task_exit()` marks the task DEAD and yields; the scheduler reaps the dead task's user page directory after switching CR3 away from it (`schedule()` reaper, `task.c`). Pool is fixed-size (`MAX_TASKS=8`).

Per-task state (`task_t` in `kernel/task.h`):
- `pid` — monotonically assigned (idle = 1, others from 2)
- `cwd[VFS_PATH_MAX]` — inherited from creator; not yet authoritative (VFS still uses `s_cwd`)
- `tty` — TTY index (TASK_TTY_NONE for unbound); not yet authoritative (vtty.c still uses `vtty_tasks[]`)
- `sig_pending` / `sig_mask` — Linux-style signal bitmasks (subsystem to follow)
- `fd_table` — opaque pointer (placeholder until per-task fd table lands)
- `user_brk`, `page_dir`, `state`, `name`, `esp`, `stack`, `next`

### Syscall ABI (`int 0x80`, Linux i386 convention)
| EAX | Syscall          | Args |
|-----|------------------|------|
| 1   | SYS_EXIT         | EBX = status |
| 3   | SYS_READ         | EBX = fd (0=stdin), ECX = buf ptr, EDX = count |
| 4   | SYS_WRITE        | EBX = fd, ECX = buf ptr, EDX = count.  fd 1 = stdout (VGA only); fd 2 = stderr (VGA + COM1 serial) |
| 100 | SYS_DEBUG        | EBX = uint32 checkpoint value (prints to VGA + serial) |
| 158 | SYS_YIELD        | — |
| 211 | SYS_WRITE_SERIAL | EBX = buf ptr, ECX = count.  COM1-only (no framebuffer) |

### Keyboard (Phase 2)
- Arrow-key sentinels at `0x80`–`0x83` (no longer collide with Ctrl codes).
- Per-task 64-byte ring buffers (up to `KB_TASK_SLOTS=4` tasks).
- `keyboard_getchar()` registers the calling task and dequeues from its slot.
- `keyboard_poll()` non-blocking version; falls back to global ring if no slot.
- Ctrl+A prefix arms the pane-switch dispatcher (Ctrl-A,U / Ctrl-A,J).
- Ctrl+C: sets `g_sigint=1` AND routes `\x03` to the focused task's queue.
- `keyboard_sigint_consume()` atomically reads and clears `g_sigint`.

### Shell features
- Inline editing (cursor movement, insert at point).
- History navigation (↑/↓ arrows), up to 16 entries.
- Ctrl+C: abort current input line (prints `^C`, returns empty line to REPL).
- Tab completion: first token completes command names; subsequent tokens complete VFS paths via `vfs_complete()` → `fat32_complete()`.
- `exec <path>`: loads and runs an ELF binary from the VFS. Ctrl+C during exec force-kills the child task.

### VMM (per-task page directories)
- `vmm_create_pd()` — allocates a page directory and mirrors kernel PDEs (indices 0–63)
- `vmm_map_page(pd, vaddr, paddr, flags)` — installs 4 KiB mapping; creates page tables on demand with `PAGE_USER`
- `vmm_switch(pd)` — loads CR3

### Ring-3 entry (`ring3.S`)
`ring3_enter(entry, stack_top)` loads user data selector (0x23) into DS/ES/FS/GS, builds a 5-word `iret` frame (SS=0x23, ESP, EFLAGS|IF, CS=0x1B, EIP), and executes `iret`. Never returns. Caller must call `tss_set_kernel_stack()` and `vmm_switch(pd)` first.

### Userspace apps (`src/userspace/`)
Freestanding ELF binaries built with the cross-compiler. Link against `crt0.S` + `link.ld`. Loaded and executed by `elf_exec()` (shell `exec` command). Available apps:

| Binary | Description |
|--------|-------------|
| `hello.elf` | Hello-world smoke test |
| `calc.elf` | bc-style expression calculator — `+`, `-`, `*`, `/`, `%`, parentheses, recursive-descent parser |

### ktest harness
`KTEST_ASSERT(expr)` — records pass/fail to VGA + serial.
`KTEST_ASSERT_EQ(a, b)` — equality variant.
`KTEST_ASSERT_MAJOR(expr)` — like `KTEST_ASSERT` but calls `kpanic_at` on failure; use for invariants whose violation indicates kernel corruption (GDT validity, PMM sanity, etc.).

## Debug output
- VGA: `t_writestring`, `t_hex`, `t_dec`, `t_putchar` (`include/kernel/tty.h`)
- Serial: `Serial_WriteString`, `Serial_WriteHex` (`include/kernel/serial.h`)
- `KLOG` / `KLOG_HEX` macros — serial only, require `-DDEV_BUILD` compile flag (no-ops in release)
- `SYS_DEBUG` writes to **both** VGA and serial unconditionally — preferred for ring-3 debugging
- `kpanic(msg)` / `KPANIC(msg)` / `kpanic_at(msg, file, func, line)` — renders a panic screen and halts

## Key source layout
```
src/kernel/arch/i386/
  boot/       boot.S (Multiboot 2 entry), crti/crtn
  core/       GDT/IDT (descr_tbl.c), ISR stub (isr_asm.S), interrupt dispatch (isr.c)
  mm/         pmm.c (frame allocator), paging.c, vmm.c (per-task page dirs), heap.c
  drivers/    serial, keyboard, timer, IDE, ACPI, partition
  fs/         fat32.c, iso9660.c, vfs.c
  display/    tty.c (VGA text), vesa.c + vesa_tty.c (VESA framebuffer)
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
| **ELKS** | GPLv2 | Minimal libc / crt0 model; `vics` editor philosophy |
| **FUZIX** | GPLv2 | vi-style editor design; libc porting approach for small systems |
| **CP/M** | Historic | Terminal-owns-screen philosophy; self-contained program model |
| **musl libc** | MIT | Target libc for future userspace; syscall stub conventions |
| **lwIP** | BSD | Future TCP/IP stack candidate |
| **GRUB** | GPLv2 | Bootloader; Multiboot 2 tag format |
| **OSDev wiki** | CC-BY-SA | Cross-compiler setup, paging, descriptor table guidance |

## Current state (as of May 2026)

Makar boots to an interactive VESA shell with 4 independent TTYs.
Alt+F1–F4 switches between them; each is a separate cooperative kernel task
with its own stack.  The major subsystems in place:

- **Display**: VESA framebuffer (Bochs VBE, defaults to 720p), VGA text fallback (80×50). Pane abstraction (`vesa_pane_t`) used by VICS and the TTY manager.
- **Multi-TTY**: 4 shell tasks (`shell0`–`shell3`). Alt+F1–F4 switches focus; `vtty.c` routes keyboard input and sends `KEY_FOCUS_GAIN` to the newly active task. Each task redraws on focus gain.
- **VICS**: Pane-aware text editor. Derives column/row counts from the active `vesa_pane_t` at runtime — works correctly at any VESA resolution. Modelled on ELKS/FUZIX vi: lightweight, stable, no heap after startup.
- **Storage**: FAT32 (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. VFS layer with CWD, auto-mount. Full read/write/delete/rename support on FAT32.
- **Userspace**: Ring-3 protected mode via `iret`. ELF loader (`elf_exec`). Syscalls: exit, read, write, open, close, lseek, brk, debug, yield + Makar extensions (208–210). Apps: `calc.elf`, `hello.elf`, `ls.elf`, `echo.elf`, `vics.elf`, `diskinfo.elf`, `rm.elf`, `mv.elf`, `cp.elf`.
- **Shell**: Inline editing, history, tab completion, Ctrl+C sigint. `lsman` / `man <cmd>` replace `help`. Built-in file ops: `rm`, `rmdir`, `mv`.
- **Tasking**: Cooperative round-robin. Background ktest at boot (runs before any shell prompt appears).
- **GRUB**: Two-entry menu (Makar OS + Next available device), 5-second timeout.

## Active branches

### `feat/userspace-fileops` (current)
FAT32 delete/rename APIs, VFS wrappers, syscalls 208–210, shell built-ins `rm`/`rmdir`/`mv`, and standalone ELFs `rm.elf`, `mv.elf`, `cp.elf`.

## Future roadmap

### Near-term kernel work
- **Runtime test_mode via cmdline**: `MULTIBOOT2_TAG_TYPE_CMDLINE` already parsed in `kernel.c`. Next: replace remaining `#ifdef TEST_MODE` guards with runtime `if (test_mode)`.
- **Preemptive scheduling hardening**: timer preemption is wired (PIT every 4 ticks). Pending: interrupt-safe critical sections inside `schedule()`, per-task tick accounting, runtime-tunable quantum, and a ktest that proves preemption (busy-loop tasks make progress without cooperative yields).
- **Signals**: full `kill()`/`signal()` ABI beyond the current Ctrl+C `g_sigint` flag.

### Userspace / libc porting

The long-term goal is a self-hosting userspace. Prerequisites and approach:

1. **musl libc** (preferred over glibc or uClibc for size):
   - Needs: `mmap`/`munmap`, `brk`/`sbrk`, `read`/`write`/`open`/`close`/`stat`, `fork`/`exec`/`wait` (or at minimum `posix_spawn`), `getpid`, signals.
   - Current blocker: no `fork` — Makar has cooperative tasks, not POSIX processes. Either implement `fork` (requires COW page tables) or target a no-fork musl config (`musl` + `MUSL_NO_FORK` equivalent).
   - Recommended path: add `SYS_OPEN`, `SYS_CLOSE`, `SYS_READ` (file), `SYS_WRITE` (file), `SYS_STAT`, `SYS_LSEEK` first, then `SYS_BRK` (heap extension), then `SYS_MMAP` (anonymous), then attempt musl.

2. **uClibc-ng** (lighter than musl, targets embedded — no fork required for static linking):
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
   - Alternative: implement `posix_spawn` semantics (create + exec without fork) — sufficient for a non-interactive shell and simpler to implement.

### Hardware / platform
- **USB HID keyboard**: currently PS/2 only. QEMU emulates PS/2 by default; real hardware may need USB HID via OHCI/EHCI.
- **Network**: RTL8139 driver → lwIP → DHCP/DNS → wget/curl-lite. See Next PR section above.
- **64-bit (x86-64)**: significant rewrite — new GDT/IDT, long mode entry, 64-bit paging. Worth considering once userspace is stable on i386.

## Documentation

- `docs/userland-libc.md` — how to build and link a freestanding libc for Makar userspace; step-by-step from syscall fixes through musl/uClibc-ng to TCC in-kernel compilation. Includes FOSS attribution and OSDev wiki references.

---

## Next: "Serious dev work in-place" (write, compile, run C on a live Makar system)

See `SURVEY.md` for complete inventory of shell commands, userspace apps, VFS/FAT32 APIs, and the installer.

### Kernel prerequisites (must land first)
1. **`SYS_WRITE(fd, buf, len)`** — fix EAX=4 to standard Linux i386 convention (fd + buffer + length). Unblocks all libc stdio.
2. **`SYS_GETCWD` + `SYS_READDIR`** — needed for shell tab completion and `ls` from userspace.

### Libc / toolchain
3. **musl static link** — once the fd table and `SYS_BRK` exist, a musl static binary compiles with the existing i686-elf cross-compiler. See `docs/userland-libc.md` for the step-by-step.
4. **uClibc-ng** as a lighter fallback if musl proves difficult without `fork`.

### In-kernel compiler
5. **TCC (Tiny C Compiler)** — ~200 KiB, compiles C to ELF in memory, writes output via `vfs_write_file`. No `fork` needed. Enables write-compile-run on bare metal, CP/M-style.

### Networking (longer-term, same PR series)
6. **NIC driver** — RTL8139 is the primary target (well-documented, QEMU `-device rtl8139`). AMD PCNet (`-device pcnet`) is the QEMU default and also well-documented.
7. **lwIP** — BSD-licensed, small footprint, designed for embedded. Needs a `sys_arch` adapter and a packet Rx/Tx hook from the NIC driver.
8. **DHCP + DNS stubs** — lwIP includes both; just need the netif glue.
9. **wget/curl-lite** — a minimal HTTP GET over lwIP. No TLS initially; TLS via mbedTLS or BearSSL later.

### Process model (prerequisite for userland shell)
10. **`fork()` or `posix_spawn`** — COW page-table clone (or simpler: exec-without-fork via `posix_spawn` semantics). Required before moving the shell to userland. See `docs/userland-libc.md` roadmap graph.
