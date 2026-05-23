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
- `cwd[VFS_PATH_MAX]` - authoritative per-task working directory; inherited from creator on `task_create`; `vfs_getcwd()` / `vfs_cd()` route here through `task_current()`. Pre-tasking-init, `vfs.c` falls back to `s_boot_cwd`, which `tasking_init` then hands off to `idle->cwd`. VT0 at `/proc` and VT1 at `/mnt/cdrom/apps` are fully independent.
- `tty` - TTY index (TASK_TTY_NONE for unbound); not yet authoritative (vtty.c still uses `vtty_tasks[]`)
- `sig_pending` / `sig_mask`  Linux-style signal bitmasks (subsystem to follow)
- `fd_table` - per-task fd table (`kernel/fd.h`); fds 0/1/2 pre-bound to stdin/stdout/stderr at `task_create`.  `FD_KIND_FILE` slots hold a growable kmalloc'd buffer plus `capacity`/`dirty`/`writable`/`append`/inline `path[VFS_PATH_MAX]`; `fd_close` flushes dirty buffers via `vfs_write_file(path,...)`.  `fd_table_clone` (fork) deep-copies each FILE buffer and **clears `dirty` on the child copy** so only the parent's eventual close re-writes the file — a deliberate non-POSIX shortcut, undone by the future `open_file_t` refactor (pipe(2)/dup(2) slice).
- `exec_params` - kmalloc'd `exec_params_t` set by `shell_exec_elf` and consumed by `exec_task_entry`. Per-task so two shells on different TTYs can `exec` concurrently without trampling each other's argv/path (the prior static-globals approach caused a `CS=0x3F8` ring-3 panic under load); reaped on slot reuse.
- `user_brk`, `page_dir`, `state`, `name`, `esp`, `stack`, `next`

### Syscall ABI (`int 0x80`, Linux i386 convention)
Authoritative table in `src/kernel/include/kernel/syscall.h`. Selected entries:

| EAX | Syscall          | Args |
|-----|------------------|------|
| 1   | SYS_EXIT         | EBX = status.  Sets `task_current()->exit_status` before transitioning to ZOMBIE/DEAD (see SYS_WAIT4). |
| 2   | SYS_FORK         | -.  COW-clone the calling task; returns child pid in parent, 0 in child, -EAGAIN on failure. (slice 15) |
| 3   | SYS_READ         | EBX = fd (0=stdin keyboard, ≥3=VFS), ECX = buf, EDX = count |
| 4   | SYS_WRITE        | EBX = fd, ECX = buf, EDX = count. fd 1 = VGA, fd 2 = VGA + COM1, ≥3 = VFS.  A `FD_KIND_BLOCKDEV` fd writes via `devfs_pwrite` at the fd's byte offset.  A writable `FD_KIND_FILE` fd mutates its in-memory buffer (krealloc grow, geometric doubling); the dirty buffer is flushed back via `vfs_write_file(path, ...)` on `SYS_CLOSE`. |
| 5   | SYS_OPEN         | EBX = path, ECX = flags (`O_RDONLY/WRONLY/RDWR` ∨ `O_CREAT 0100` ∨ `O_TRUNC 01000` ∨ `O_APPEND 02000`; Linux i386 values).  Returns fd.  `/dev` block devices bind as `FD_KIND_BLOCKDEV` (no eager buffer); other paths eager-buffer existing content up to `SYSCALL_FILE_MAX` (8 MiB).  `O_CREAT` creates an empty buffer when the path doesn't exist; `O_TRUNC` discards the eager-load and starts empty.  The opened path is kept inline on the fd slot so the close-time flush doesn't need a second lookup. |
| 6   | SYS_CLOSE        | EBX = fd.  Flushes any dirty `FD_KIND_FILE` buffer via `vfs_write_file`; returns -1 if the backend rejects the flush (the buffer is freed regardless). |
| 106 | SYS_STAT         | EBX = path, ECX = `struct stat *`.  Populates `st_mode`/`st_size`/`st_nlink`/`st_blksize`/`st_ino`; other fields zero-filled.  `st_ino` is a stable-per-boot FNV-1a-32 hash of the path. |
| 108 | SYS_FSTAT        | EBX = fd, ECX = `struct stat *`.  Same shape as SYS_STAT; for `FD_KIND_FILE` `st_size` reflects pending (unflushed) writes. |
| 11  | SYS_EXECVE       | EBX = path, ECX = argv (NULL-terminated `char *const argv[]`), EDX = envp (ignored).  On success doesn't return.  (slice 16a) |
| 19  | SYS_LSEEK        | EBX = fd, ECX = offset, EDX = whence (works on `FD_KIND_FILE` and `FD_KIND_BLOCKDEV`) |
| 37  | SYS_KILL         | EBX = pid, ECX = signo |
| 45  | SYS_BRK          | EBX = new break (returns current/new break) |
| 48  | SYS_SIGNAL       | EBX = signo, ECX = handler (returns previous handler) |
| 100 | SYS_DEBUG        | EBX = uint32 checkpoint (prints to VGA + serial) |
| 114 | SYS_WAIT4        | EBX = pid (-1 = any child), ECX = `int *status`, EDX = options (WNOHANG=1), ESI = rusage ptr (ignored).  Returns child pid, 0 (WNOHANG no zombie), or -ECHILD.  (slice 16b) |
| 119 | SYS_SIGRETURN    | - (sigframe trampoline, not for direct use) |
| 158 | SYS_YIELD        | - |
| 200 | SYS_GETKEY       | raw single-char keyboard read |
| 201–204 | SYS_PUTCH_AT / SET_CURSOR / TTY_CLEAR / TERM_SIZE | direct TTY ops for full-screen apps (vix) |
| 218 | SYS_CARET_STYLE | set VESA caret style (0=line, 2=flashing block); returns previous.  No-op in VGA-text mode.  Used by vix.elf |
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
| `basic.elf` | C64-flavoured line-numbered **integer** BASIC.  `basic` (REPL) or `basic prog.bas` (load + RUN).  PRINT/LET/IF..THEN/GOTO/GOSUB/RETURN/FOR..NEXT/INPUT/REM/END/CLS/PAUSE + graphics PLOT/LINE/RECT/COLOR + `XMAX`/`YMAX` screen-size functions.  Ctrl-C is RUN/STOP (breaks a running program back to the `READY.` prompt; a second Ctrl-C at the prompt, or Ctrl-C in file mode, exits) (via `SYS_DRAW_LINE`, native VESA res; `YMAX` excludes the makmux status row so full-screen fills don't clip it; "?NO GRAPHICS" in VGA text mode).  Integer-only because the kernel doesn't init/save the x87 FPU — fractional work uses fixed-point.  Ships `mandelbrot.bas` + `lines.bas` type-in samples in `/mnt/cdrom/apps` |
| `fdisk.elf` | MBR partition editor (line-driven, scriptable) — opens a `/dev` block device, edits the four primary entries (`p`/`n`/`d`/`t`/`a`/`w`/`q`).  `fdisk /dev/hda` (defaults to `/dev/hda`).  Size tokens: `max`, `N%` (of the whole disk), `NM`/`NG`, bare sectors; clamped to disk end.  Writes the 512-byte MBR back through the block-device fd; devfs's read-modify-write preserves the bootstrap code |
| `cfdisk.elf` | Full-screen cfdisk-style MBR editor (the `cfdisk` command) — partition/free-space table with arrow-key row selection and a bottom action bar (`Bootable`/`Delete`/`New`/`Type`/`Write`/`Quit`).  MBR primary-only.  Type picker accepts names (`fat32`/`ext2`/`swap`/`ntfs`) or hex.  Clean-room (no util-linux source); renders via the same full-screen syscalls as `vix` and respects the makmux status row |
| `vix.elf` | vi-style text editor (the `vix` command — runs as its own ring-3 task, shows in maktop).  Vim-style line-number gutter, word wrap with `+` continuation markers, `~` past-EOF rows, flashing block caret (`SYS_CARET_STYLE`), resolution-agnostic via `SYS_TERM_SIZE`; uses `SYS_PUTCH_AT` / `SYS_SET_CURSOR`.  Ctrl+S save, Ctrl+Q quit (double-press when dirty).  Replaced the former in-kernel `vix` builtin (`proc/vix.c`, removed) |
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
  fs/         fat32.c, iso9660.c, procfs.c (synthetic /proc), devfs.c (synthetic /dev block devices), vfs.c
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

## Companion reference files

To keep this file focused on day-to-day work, longer-lived material lives alongside it:

- **`CLAUDE.history.md`** — current subsystem state (May 2026), recently-merged PR log, and FOSS attribution. Consult for "what's already shipped / what does subsystem X do today".
- **`CLAUDE.roadmap.md`** — the slice queue (done + open), userspace/libc porting plan, hardware/platform notes, and the "serious dev work in-place" (compiler/networking) roadmap. Consult when planning new features or asked about direction.

Published docs: `docs/userland-libc.md` (freestanding libc + TCC path), `SURVEY.md` (full inventory of shell commands / apps / VFS APIs / installer).
