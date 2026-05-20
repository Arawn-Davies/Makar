# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Makar is a hobby x86 (i386) bare-metal OS kernel written in C and AT&T assembly, booted via GRUB Multiboot 2. It targets 32-bit protected mode and runs in QEMU. Docker wraps the full build/test toolchain - no host cross-compiler is required.

## Build commands

All build, test, and boot operations go through a single entrypoint:

```sh
# Day-to-day (build + run in one shot)
./run.sh iso boot       # clean → debug ISO → interactive QEMU
./run.sh iso test       # full CI suite: ktest + GDB boot-checkpoint tests
./run.sh ktest graphical  # test ISO → ktest with display window (needs host QEMU)
./run.sh iso release    # optimised release ISO

./run.sh hdd boot       # clean → build kernel → HDD image → interactive QEMU
./run.sh hdd test       # clean → build kernel → HDD image → GDB boot test
./run.sh hdd release    # HDD image only

./run.sh ui        # black-box UI tests (headless QEMU, sendkey + serial grep)
./run.sh ui graphical    # same but with visible QEMU window + paced typing

# CI-style split modes (build once, run many — used by .github/workflows/build-test.yml)
./run.sh iso build      # kernel + makar.iso + makar-test.iso, no run
./run.sh hdd build      # kernel + makar-hdd-test.img, no run
./run.sh ktest      # ktest against existing makar-test.iso
./run.sh gdb iso    # GDB ISO boot test against existing makar.iso
./run.sh gdb hdd    # GDB HDD boot test against existing makar-hdd-test.img

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
./run.sh iso test
# Phase 1: test_mode ISO → ktest_run_all() → QEMU exits.  Output: ktest.log
# Phase 2: debug ISO + FAT32 test disk → full GDB test suite.  Output: gdb-test.log
# exits 0 on pass, 1 on any failure
```

**HDD boot test:**
```sh
./run.sh hdd test
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

**Black-box UI tests** (`tests/ui_test.sh`, fronted by `./run.sh ui` / `ui-test-gui`): boots `makar.iso`, drives keyboard input through QEMU's **HMP** (Human Monitor Protocol — the text-based control channel exposed by `-monitor unix:...`) via the `sendkey` command, and asserts on substrings in the serial mirror. Covers user-visible flows that `iso-test` doesn't: ELF exec → syscalls → output, shell tab completion, glob expansion, `cd`/`pwd`. **Not wired into CI** (the per-merge job was dropped in `a9b7474` — the framework's reliance on HMP timing made it flaky under the **TCG** (Tiny Code Generator — QEMU's interpreted/JIT CPU emulator, used because KVM is off by default per the note above) emulation that runs in the CI containers). Run locally before opening any PR that touches syscalls, shell, ELF exec, VFS, keyboard, or display:
```sh
./run.sh ui                                # headless: all scenarios
./run.sh ui exec-hello                     # headless: one scenario
./run.sh ui graphical                            # visible window + paced typing (watch it run)
./run.sh ui graphical exec-hello                 # one scenario, visible
QEMU_DISPLAY=cocoa ./run.sh ui graphical         # override QEMU display backend (cocoa|gtk|sdl)
KEY_DELAY=0.3      ./run.sh ui graphical         # slower typing (default 0.15 s/key)
UI_TEST_LOGDIR=/tmp/uilogs ./run.sh ui     # keep logs (serial + PPM screen dump)
```
The `ui-test-gui` target keeps a paced-typing visible-window mode for debugging; headless `ui-test` is the canonical "did the change regress anything" path and is what runs noise-free. **Shutdown path differs by mode**: headless sends HMP `quit` (instant), while GUI mode types `shutdown<Enter>` into the focused shell so the kernel runs its real ACPI S5 power-off (port `0x604 / 0x2000` — see `acpi_shutdown()`), then QEMU exits naturally; this exercises the shutdown code path on every GUI run *and* gives the watcher a visible "Shutting down..." final frame instead of the window blinking out the instant assertions complete. Both modes fall back to SIGKILL after a bounded wait if the guest is wedged. **PPM** = Portable Pixmap, the screen-snapshot format HMP's `screendump` emits — useful for triaging visual-only regressions (cursor position, gutter rendering) that the serial mirror can't capture. Scenarios live as `scenario_<name>` shell functions in `tests/ui_test.sh`; add a new one alongside any PR that changes a user-facing path.

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
- `cwd[VFS_PATH_MAX]` - authoritative per-task working directory; inherited from creator on `task_create`; `vfs_getcwd()` / `vfs_cd()` route here through `task_current()`. Pre-tasking-init, `vfs.c` falls back to `s_boot_cwd`, which `tasking_init` then hands off to `idle->cwd`. VT0 at `/proc` and VT1 at `/cdrom/apps` are fully independent.
- `tty` - TTY index (TASK_TTY_NONE for unbound); not yet authoritative (vtty.c still uses `vtty_tasks[]`)
- `sig_pending` / `sig_mask`  Linux-style signal bitmasks (subsystem to follow)
- `fd_table` - per-task fd table (`kernel/fd.h`); fds 0/1/2 pre-bound to stdin/stdout/stderr at `task_create`
- `exec_params` - kmalloc'd `exec_params_t` set by `shell_exec_elf` and consumed by `exec_task_entry`. Per-task so two shells on different TTYs can `exec` concurrently without trampling each other's argv/path (the prior static-globals approach caused a `CS=0x3F8` ring-3 panic under load); reaped on slot reuse.
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
| 215 | SYS_GETCWD       | EBX = char *buf, ECX = size. Copies calling task's cwd; returns strlen or -1 |

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
- **makbox fallback is restricted**: bare command names route through makbox **only** if they match an actual applet (`ls cat cp mv rm rmdir echo pwd`). Anything else hits the shell's "Unknown command" path — typos no longer trigger makbox's usage banner.
- `datetime` / `date` / `time` builtins — one-line `YYYY-MM-DD HH:MM:SS` from `/proc/rtc`.  Scriptable; for fullscreen use see `clock.elf`.

### Shell scripting (sh-flavoured)
The kernel shell exposes a per-shell-task scripting layer (`kernel/sh_script.h`, `arch/i386/shell/sh_script.c`):

| Surface | Behaviour |
|---|---|
| `NAME=value` | Per-task assignment.  RHS shell-expanded.  Stored in `task_t.script_vars` (isolated per VT — VT0's vars don't leak into VT1, matching the per-VT palette model). |
| `$VAR` / `${VAR}` / `$?` | Expansion at REPL or inside scripts.  `$?` is the last command's exit status (set after every dispatched line and every `[ TEST ]`). |
| `env` / `unset NAME ...` | Dump table / remove vars. |
| `read VAR` | Reads one line of input from the keyboard into VAR. |
| `[ TEST ]` | String tests (`-z`/`-n`/`=`/`!=`) and integer tests (`-eq`/`-ne`/`-lt`/`-le`/`-gt`/`-ge`).  Non-numeric operand to integer ops fails with `[: integer expected`. |
| `sh script.sh` / `./script.sh` | Run a script file (path ending in `.sh` dispatches through the script interpreter; arbitrary paths still try to ELF-exec). |
| `# comment` | End-of-line comments (outside quotes). |
| `if / elif / else / fi` | Chained, both multi-line and single-line `if [ X ]; then CMD; fi` forms. |
| `while ... do ... done` | Multi-statement `do` bodies via `;`-split preprocessor. |
| `for VAR in WORDS; do ... done` | Word-list iteration with `$VAR` expansion in the list. |
| `sleep N` | Busy-yield until N seconds elapse (PIT-driven). |
| `true` / `false` | POSIX status helpers. |

Limitations: no command substitution (`$(cmd)`), no pipes, no subshells (needs `fork()` — see slice 12).  See `src/userspace/demo.sh` for a worked example exercising every surface.

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
| `makbox.elf` | Makar busybox: multicall binary for `ls`, `cat`, `cp`, `mv`, `rm`, `rmdir`, `echo`, `pwd`. Shell dispatch falls back to `makbox <name>` **only for those specific applet names** — random typos no longer get routed into makbox just to surface its usage banner; they hit the shell's "Unknown command" path instead. Replaces the former standalone `ls.elf`/`echo.elf`/`rm.elf`/`mv.elf`/`cp.elf`. |
| `clock.elf` | Fullscreen wall-clock display (CMOS RTC via `/proc/rtc`).  For scripted / one-line use see the `datetime`/`date`/`time` shell builtins instead. |
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
              (fs/ keeps mount/umount/cd/mkdir/mkfs/isols/write/touch; ls/cat/cp/mv/rm/pwd/echo are makbox applets)
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

- **Display**: VESA framebuffer (Bochs VBE, defaults to 720p), VGA text fallback (80×50). Pane abstraction (`vesa_pane_t`) used by VIX. Per-TTY logical character grid (`vt_buf_t` in `display/vt.c`) backs every shell — writes go to the grid first; the framebuffer is only painted when that TTY is focused. After any "fullscreen" shell command returns (vix, install, any ELF launched via `exec` or PATH), `shell_dispatch` calls `shell_restore_screen()` which repaints the focused VT's grid to the FB — so post-exit screen is never blank.
- **Multi-TTY**: 4 shell tasks (`shell0`–`shell3`). `vtty.c` routes keyboard input via `task_t.tty` (authoritative) and tracks the focused slot. `vtty_switch()` defers the framebuffer repaint out of IRQ context to `vtty_drain_pending()`, which runs from the destination shell's `keyboard_getchar` poll loop. A tmux-style status bar lives in the reserved bottom row showing `Makar  VT0  VT1  VT2  VT3  ...  Alt+F1-F4` with the active slot highlighted.
- **VIX**: Pane-aware text editor. Derives column/row counts from the active `vesa_pane_t` at runtime - works correctly at any VESA resolution. Modelled on ELKS/FUZIX vi: lightweight, stable, no heap after startup.
- **Storage**: FAT32 (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. VFS layer with CWD, auto-mount. Full read/write/delete/rename support on FAT32. Synthetic `/proc` mount exposes `cpuinfo`, `meminfo`, `tasks`, `uname` as read-only files generated on demand.
- **Tasking**: Round-robin scheduler with timer-driven preemption (PIT 100 Hz, `SCHED_QUANTUM = 4` ticks → 40 ms slice). Per-task `pid`, `cwd`, `tty`, signal bitmasks, and real per-task fd table (`fd_table_t` in `kernel/fd.h`, 16 slots, fds 0/1/2 pre-bound to stdin/stdout/stderr). User PD reaped on task exit, fd table reaped on slot reuse. Background ktest harness runs before the shell prompt appears.
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
| #129 | `feat/per-tty-buffers` | Per-TTY `vt_buf_t` backing grids, deferred FB repaint on Alt+Fn switch, tmux-style status bar at bottom row, synthetic `/proc` filesystem, glob + tab completion across VFS, MAKAR_VERSION single-source, v0.5.0 |
| #130 | `feat/vics-vim-polish` | VIX rename (was VICS, C-Sharp acronym is dead), vim-style gutter + word wrap + flashing block caret, root `/` enumeration in `vfs_complete`, linux-like serial (`g_serial_verbose`, `console=ttyS0`, `verbose` builtin), UI-test framework (`tests/ui_test.sh`) wired into CI as a 4th parallel job, shell-side FB restore after fullscreen commands |

## Future roadmap

### Slice queue (`feat/tty-multitasking` → follow-ups)

Tracked here, pulled into branches one at a time so each PR stays focused.

### Done

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
| 16b | **`SYS_WAIT4` (EAX=114) + `TASK_ZOMBIE` state** — child holds its pool slot + `exit_status` until parent reaps it; `parent_pid` + `exit_status` on `task_t`; orphan zombies auto-reaped by parent's `task_exit`.  `forktest.elf` + `execvetest.elf` now wait4 instead of busy-yielding. | ✅ local |

### Open

| # | Slice | Status |
|---|---|---|
| 17 | **UTF-8 terminal** with ASCII fallback / runtime mode switch | ⏭ deferred |
| 18 | **`ps`-style task listing** with privilege/state/CWD/TTY columns (today's `cat /proc/tasks` covers this; promote only if richer column control is needed) | ⏭ deferred |
| 19 | **VGA-fallback per-TTY** — route `tty.c` writes through `vt_buf` so VGA-text mode gets the same per-TTY isolation that VESA already has | ⏭ |
| 20 | **Userland shell (full parity, multi-PR feature)** — lift the in-kernel shell (~4500 lines across shell.c + shell_cmd_*.c + sh_script.c + shell_glob.c + shell_help.c) into a ring-3 ELF (`sh.elf`).  Multi-PR feature, NOT a single slice.  Plan paused; do not start without re-confirming scope.  Each sub-PR is runnable in isolation; kernel shell stays the default until the final flip.  See 20a..20f. | ⏭ paused |
| 20a | `sh.elf` skeleton — read-parse-fork-execve-wait4 loop; builtins `cd` / `pwd` / `exit`.  New kernel shell builtin `usermode` invokes `exec /apps/sh.elf` so it can be smoke-tested without touching the kernel shell. | ⏭ |
| 20b | Inline editing + history (arrow keys via `SYS_GETKEY` raw mode + sentinels, backspace, `!!`).  Mirrors `shell.c`'s `shell_readline`. | ⏭ |
| 20c | Variables + expansion: `NAME=value`, `$VAR` / `${VAR}` / `$?`, `env` / `unset` / `read` builtins.  Mirrors `sh_vars.c` + `sh_script.c`. | ⏭ |
| 20d | Control flow: `if` / `elif` / `else` / `fi`, `while`, `for ... in`, `[ TEST ]`, `true`/`false`/`sleep`.  Mirrors the rest of `sh_script.c`. | ⏭ |
| 20e | Tab complete + glob expansion.  Needs a streaming `SYS_READDIR` first (today's `SYS_LS_DIR` returns a pre-rendered text blob); that lands as its own micro-slice. | ⏭ |
| 20f | Boot wires `sh.elf` per VT (replaces the four in-kernel `shell0..shell3` tasks).  Kernel shell stays in-tree as a `/apps/sh.elf` fallback for boots where the ELF is missing.  **The boot-path commit; everything before it leaves the existing shell untouched.** | ⏭ |
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
- **Network**: RTL8139 driver → lwIP → DHCP/DNS → wget/curl-lite. See Next PR section above.
- **64-bit (x86-64)**: significant rewrite - new GDT/IDT, long mode entry, 64-bit paging. Worth considering once userspace is stable on i386.

## Documentation

- `docs/userland-libc.md` - how to build and link a freestanding libc for Makar userspace; step-by-step from syscall fixes through musl/uClibc-ng to TCC in-kernel compilation. Includes FOSS attribution and OSDev wiki references.

---

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
