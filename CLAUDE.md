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
./run.sh gui       # same but with visible QEMU window + paced typing (replaces the old "ui graphical" form; arg order no longer matters)

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

**In-kernel UI tests (`src/userspace/incore.sh`)**: a shell-script test driver that runs the non-interactive UI scenarios (hello, forktest, execvetest, alloctest) directly from inside the kernel via the kernel sh interpreter.  Each test invokes its ELF and branches on `$?` (the ELF's own exit status) instead of HMP+serial-grep round-trips; the final marker `INCORE: ALL PASS` (or `INCORE: FAIL`) is what the runner asserts on.  Fronted by the single HMP scenario `test_incore` (`./run.sh ui incore`).  Faster than per-test HMP, no typing races, and the test logic lives in a `.sh` file you can edit without touching the runner.  Tradeoff: loses per-scenario screendump evidence on panic, so only use for tests that don't depend on framebuffer state.  Interactive features (TAB, Ctrl-C, VT switching, sh.elf readline, fullscreen apps) stay in HMP-driven scenarios where the keyboard event itself is under test.

**Black-box UI tests** (`tests/ui_test.sh`, fronted by `./run.sh ui` / `ui-test-gui`): boots `makar.iso`, drives keyboard input through QEMU's **HMP** (Human Monitor Protocol — the text-based control channel exposed by `-monitor unix:...`) via the `sendkey` command, and asserts on substrings in the serial mirror. Covers user-visible flows that `iso-test` doesn't: ELF exec → syscalls → output, shell tab completion, glob expansion, `cd`/`pwd`. **Not wired into CI** (the per-merge job was dropped in `a9b7474` — the framework's reliance on HMP timing made it flaky under the **TCG** (Tiny Code Generator — QEMU's interpreted/JIT CPU emulator, used because KVM is off by default per the note above) emulation that runs in the CI containers). Run locally before opening any PR that touches syscalls, shell, ELF exec, VFS, keyboard, or display:
```sh
./run.sh ui                                # headless: all scenarios
./run.sh ui fast                           # headless: dev inner-loop subset
./run.sh ui libc                           # headless: TCC self-rebuild scenarios
./run.sh ui incore                         # headless: in-kernel sh.script driver (hello/forktest/execvetest/alloctest)
./run.sh ui shell|cd|fs|posix|vt|bughunt   # other named scenario groups
./run.sh ui exec-hello                     # headless: one scenario
./run.sh gui                               # visible window + paced typing (watch it run)
./run.sh gui exec-hello                    # one scenario, visible
./run.sh gui libc                          # libc group, visible (good for watching TCC compile)
QEMU_DISPLAY=cocoa ./run.sh gui            # override QEMU display backend (cocoa|gtk|sdl)
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
4. Timer (100 Hz PIT), keyboard (layered PS/2 driver), IDE
5. Multiboot2 cmdline parse: extracts `test_mode`, `console=ttyS0`, and `root=<spec>` (spec = `/dev/hdaN` for explicit rootfs selection, `none` to skip election, anything else / NULL = auto-detect)
6. `vfs_init` → `vfs_mount_root(root_spec)` → `vfs_auto_mount` → `vfs_ensure_root_home` (see § VFS below)
7. `tasking_init` + `task_create("shell", shell_run)` + `task_create("ktest", ktest_bg_task)`
8. `syscall_init` (registers `int 0x80` handler at IDT gate DPL=3)
9. Idle loop: `task_yield` + `hlt`

### Display mode selection
At boot, `kernel_main` calls `bochs_vbe_available()`. If the Bochs VBE I/O ports respond (QEMU `-vga std`), it sets 1280×720×32 and initialises the VESA TTY at `font_scale=2` (40-col equivalent at this res). If VBE is absent (hardware or minimal QEMU config), it falls back to VGA 80×50 text mode.

The `setmode` shell command can switch freely between any supported resolution at runtime.

### Memory map
- `0x00000000–0x0FFFFFFF` (256 MiB): kernel identity window (4 MiB large pages)
- `0x40000000` (`USER_CODE_BASE`): ring-3 code page
- `0xBFFF0000` (`USER_STACK_TOP`): ring-3 stack top.  `USER_STACK_PAGES = 8`, so the stack occupies `[USER_STACK_TOP - 32 KiB, USER_STACK_TOP)` mapped eagerly at exec.  Was a single 4 KiB page until TCC's recursive-descent parser blew past it compiling sh.c.

### Tasking
Round-robin scheduler with timer-driven preemption (PIT 100 Hz; IRQ 0 yields every `SCHED_QUANTUM=4` ticks ≈ 40 ms). Cooperative `task_yield()` is also available for explicit yields. Context switch via `task_asm.S` (callee-saved + EFLAGS). `task_exit()` marks the task DEAD and yields; the scheduler reaps the dead task's user page directory after switching CR3 away from it (`schedule()` reaper, `task.c`). Pool is fixed-size (`MAX_TASKS=32`).

**Ring-3 fault handling (v0.8):** page faults and GPFs in ring 3 no longer panic the kernel.  `kill_userspace_fault` in `arch/i386/debug/debug.c` logs `[fault] PAGE FAULT in pid=N (name) EIP=... addr=... err=... -- delivering SIGSEGV` to serial, sets `exit_status = SIGSEGV & 0x7F`, and calls `task_exit` so the scheduler reaps the offender — the shell stays up and the user sees their command come back to the prompt.  Ring-0 faults (CS&3 == 0) still panic; the panic screen + serial dump now print the running task's pid+name+ring so kernel bugs are easier to triage.  Unkillable tasks (idle, shells) still panic on ring-3 faults rather than being silently killed.

Per-task state (`task_t` in `kernel/task.h`):
- `pid` - monotonically assigned (idle = 1, others from 2)
- `cwd[VFS_PATH_MAX]` - authoritative per-task working directory; inherited from creator on `task_create`; `vfs_getcwd()` / `vfs_cd()` route here through `task_current()`. Pre-tasking-init, `vfs.c` falls back to `s_boot_cwd`, which `tasking_init` then hands off to `idle->cwd`.
- `tty` - TTY slot index. `TASK_TTY_NONE (-1)` for unbound tasks (idle, ktest_bg). `VTTY_ROOT_SLOT (4)` for mak.sh0 (the hidden root console with a backing buffer, invisible to Ctrl+Tab and the makmux status bar). Slots 0–3 for makmux VT children (mak.sh1–4). Authoritative: `vtty_buf_current()` and `vtty_is_focused()` read this field to route output. `task_fork` strips `VTTY_ROOT_SLOT` from children (they inherit `TASK_TTY_NONE`) to prevent mak.sh0's buffer from being corrupted.
- `sig_pending` / `sig_mask`  Linux-style signal bitmasks (subsystem to follow)
- `fd_table` - per-task fd table (`kernel/fd.h`); fds 0/1/2 pre-bound to stdin/stdout/stderr at `task_create`.  `FD_KIND_FILE` slots hold a growable kmalloc'd buffer plus `capacity`/`dirty`/`writable`/`append`/inline `path[VFS_PATH_MAX]`; `fd_close` flushes dirty buffers via `vfs_write_file(path,...)`.  `fd_table_clone` (fork) deep-copies each FILE buffer and **clears `dirty` on the child copy** so only the parent's eventual close re-writes the file — a deliberate non-POSIX shortcut.  `FD_KIND_PIPE` slots point at a shared `pipe_ring_t` (4 KiB ring buffer + reader/writer refcounts); fork bumps the appropriate refcount instead of copying, and `fd_close` frees the ring only when both ends hit zero (PR #181).
- `disp_fg_saved` / `disp_bg_saved` - snapshot of the calling task's display palette taken at `SYS_FORK`. `SYS_WAIT4` reads these back when a `fb_touched` child is reaped (vix, maktop, kbtester, clock, makmux, etc.) and restores the parent's colour scheme before repainting the screen, so the child's palette doesn't bleed into the next shell prompt.
- `exec_params` - kmalloc'd `exec_params_t` set by `shell_exec_elf` and consumed by `exec_task_entry`. Per-task so two shells on different TTYs can `exec` concurrently without trampling each other's argv/path (the prior static-globals approach caused a `CS=0x3F8` ring-3 panic under load); reaped on slot reuse.
- `user_brk`, `page_dir`, `state`, `name`, `esp`, `stack`, `next`

### VFS (mount table, rootfs election, overlays)

The VFS is a single static mount table (`s_mounts[]` in `src/kernel/arch/i386/fs/vfs.c`, capacity `MAX_MOUNTS=16`).  Each entry is `vfs_mount_t { char mountpoint[VFS_PATH_MAX]; vfs_backend_t backend; uint8_t drive; uint32_t lba; char slot_name[]; }`.

**Routing** (`vfs_route(abs, &drv_path)`): longest-prefix-match against every entry's `mountpoint`.  Returns the matched entry's index and writes the driver-relative path (e.g. `/dev/hda1` → `/dev` mount, drv = `/hda1`).  No path-rewriting, no recursion -- pure data lookup.  Backend dispatch is a `switch (m->backend)` in helpers `backend_ls` / `backend_cd` / `backend_read_file` / `backend_write_file` / `backend_mkdir` / `backend_delete_*` / `backend_file_exists` / `backend_complete` / `backend_unmount`.

**Backends** (`vfs_backend_t` enum): `NONE` (empty mountpoint placeholder), `EXT2`, `FAT32`, `ISO9660`, `DEVFS`, `PROCFS`, `TMPFS`, `LOGFS`.  ext2 + FAT32 are single-volume drivers (one of each at a time -- enforced by `backend_in_use`).

**Boot order** (`kernel_main`):
1. **`vfs_init`** -- probe ATAPI for an ISO9660 CD-ROM; register synthetic overlays at fixed mountpoints: `/dev` (DEVFS), `/proc` (PROCFS), `/tmp` (TMPFS), `/log` (LOGFS); register CD-ROM at `/mnt/cdrom` if present; pre-register empty `/mnt/boot` + `/mnt/root` placeholders (NONE backend; the installer + auto-mount bind into these).  Build the `/dev` node table.
2. **`vfs_mount_root(spec)`** -- elect and bind the rootfs at `/`.  Spec resolution: explicit `/dev/hdaN` → `devfs_lookup` → try ext2 then FAT32 → mount at `/`; spec = `"none"` → skip; spec NULL / unrecognised → auto-detect (walk every ATA partition, ext2 → FAT32 probe each, the first whose `/usr/lib/crt0.o` exists wins); CD-ROM fallback for live boots.
3. **`vfs_auto_mount`** -- bind ATA volumes at `/mnt/root` (single-partition; or partition 1 of dual-partition installer) and `/mnt/boot` (partition 0 FAT32 of dual-partition installer; also mirrored at `/boot` for easy access).  Single-volume backends already in use (e.g. ext2 elevated to `/` by `vfs_mount_root`) cause the matching `/mnt/<name>` slot to stay empty -- the rootfs is reachable via `/` regardless.
4. **`vfs_ensure_root_home`** -- best-effort `mkdir /root` on writable rootfs (ext2/FAT32) boots; no-op on ISO9660 or no-rootfs.

**`/mnt` is virtual**: there's no entry literally at `/mnt`; `vfs_ls("/mnt")` enumerates every mount whose `mountpoint` starts with `/mnt/` (one component deep).  Same shape for `/` -- enumerates immediate-child mounts AND lists the rootfs's actual directory contents (the union, like Linux).

**Public API**: `vfs_ls`/`cd`/`cat`/`mkdir`/`read_file`/`write_file`/`delete_file`/`delete_dir`/`rename`/`file_exists`/`stat`/`complete`/`blockdev_lookup`/`blockdev_pread`/`blockdev_pwrite`/`mount_root`/`mount_hd`/`umount_hd`/`make_mountpoint`/`remove_mountpoint`/`prepare_shutdown`/`notify_cdrom_ejected`/`ensure_root_home`/`hd_mounted`/`hd_fsname`/`getcwd`/`set_boot_drive`/`auto_mount`/`init`/`klog_*` (legacy logfs shims).

**Path conventions**: rootfs at `/`; Unix paths `/usr` `/etc` `/home` `/apps` `/root` `/bin` `/src` `/docs` resolve via the rootfs's actual directory contents (no special-casing).  `/boot/...` routes to the FAT32 boot partition mirror.  `/mnt/<name>/...` routes to user-mounted volumes.  `/mnt/cdrom/...` always works (whether or not CD is also the rootfs).  `/proc`, `/dev`, `/tmp`, `/log` are first-class overlays.

**`/log` is read-only from userspace** (Linux `/var/log` model): the VFS rejects writes routed through `backend_write_file` with `"write: read-only filesystem (/log)"`.  Kernel-side `klog_write` + friends still append into the ring directly via `ring_append` -- they bypass the VFS.  Use `/tmp` for user-writable scratch (wholesale overwrite, 16 files × 512 KiB).  `/log` files are append-only rings (dmesg-style); a `fopen("w")` re-write would otherwise double the content on every run, which bit `alloctest`'s FILE* roundtrip test.

### Syscall ABI (`int 0x80`, Linux i386 convention)
Full table: **`docs/syscalls.md`**.  Authoritative number assignments: `src/kernel/include/kernel/syscall.h`.  Userspace wrappers: `src/userspace/syscall.h`.

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
- Tab completion: **zsh-style cycling** (v0.8) — first Tab on an ambiguous prefix extends to the longest common prefix; subsequent Tabs cycle through matches in place (`tc_active`/`tc_idx` state in `shell_readline`); any non-Tab key commits the current pick.  Single-match Tab still completes + appends `/` (dir) or ` ` (file).  First token completes command names; subsequent tokens complete VFS paths via `vfs_complete()` → `fat32_complete()`.
- `exec <path>`: loads and runs an ELF binary from the VFS. Ctrl+C during exec force-kills the child task.
- **makbox fallback is restricted**: bare command names route through makbox **only** if they match an actual applet (`ls cat cp mv rm rmdir echo pwd`). Anything else hits the shell's "Unknown command" path — typos no longer trigger makbox's usage banner.
- `datetime` / `date` / `time` builtins — one-line `YYYY-MM-DD HH:MM:SS` from `/proc/rtc`.  Scriptable; for fullscreen use see `clock.elf`.

### Shell scripting (sh-flavoured)
Full reference: **`docs/scripting.md`**.  Implementation: `kernel/sh_script.h`, `arch/i386/shell/sh_script.c`.  Worked example: `src/userspace/demo.sh` (ships as `/apps/demo.sh`).  In-kernel UI-test driver pattern: `src/userspace/incore.sh`.

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
| `basic.elf` | C64-flavoured line-numbered **integer** BASIC.  `basic` (REPL) or `basic prog.bas` (load + RUN).  PRINT/LET/IF..THEN/GOTO/GOSUB/RETURN/FOR..NEXT/INPUT/REM/END/CLS/PAUSE + graphics PLOT/LINE/RECT/COLOR + `XMAX`/`YMAX` screen-size functions.  Ctrl-C is RUN/STOP (breaks a running program back to the `READY.` prompt; a second Ctrl-C at the prompt, or Ctrl-C in file mode, exits) (via `SYS_DRAW_LINE`, native VESA res; `YMAX` excludes the makmux status row so full-screen fills don't clip it; "?NO GRAPHICS" in VGA text mode).  Integer-only because the kernel doesn't init/save the x87 FPU — fractional work uses fixed-point.  Ships `mandelbrot.bas` + `lines.bas` type-in samples in `/apps` |
| `fdisk.elf` | MBR partition editor (line-driven, scriptable) — opens a `/dev` block device, edits the four primary entries (`p`/`n`/`d`/`t`/`a`/`w`/`q`).  `fdisk /dev/hda` (defaults to `/dev/hda`).  Size tokens: `max`, `N%` (of the whole disk), `NM`/`NG`, bare sectors; clamped to disk end.  Writes the 512-byte MBR back through the block-device fd; devfs's read-modify-write preserves the bootstrap code |
| `cfdisk.elf` | Full-screen cfdisk-style MBR editor (the `cfdisk` command) — partition/free-space table with arrow-key row selection and a bottom action bar (`Bootable`/`Delete`/`New`/`Type`/`Write`/`Quit`).  MBR primary-only.  Type picker accepts names (`fat32`/`ext2`/`swap`/`ntfs`) or hex.  Clean-room (no util-linux source); renders via the same full-screen syscalls as `vix` and respects the makmux status row |
| `vix.elf` | vi-style text editor (the `vix` command — runs as its own ring-3 task, shows in maktop).  Vim-style line-number gutter, word wrap with `+` continuation markers, `~` past-EOF rows, flashing block caret (`SYS_CARET_STYLE`), resolution-agnostic via `SYS_TERM_SIZE`; uses `SYS_PUTCH_AT` / `SYS_SET_CURSOR`.  Ctrl+S save, Ctrl+Q quit (double-press when dirty).  Replaced the former in-kernel `vix` builtin (`proc/vix.c`, removed) |
| `kbtester.elf` | keyboard diagnostic — logs every event (scancode/keycode/sentinel/modifier) to serial via `SYS_WRITE_SERIAL` |
| `tcc.elf` | TinyCC v0.9.27 — in-OS C compiler.  `tcc hello.c -o hello.elf` compiles a C source to a Makar-loadable ELF; `exec hello.elf` runs it.  Sysroot: `/usr/include/` (libc headers), `/usr/include/kernel-build/` (kernel headers re-exposed for the rebuild path), `/usr/lib/` (`crt1.o` + `libc.a`), `/usr/lib/tcc/` (`libtcc1.a` + TCC builtins, including the Makar `stdint.h`/`limits.h` stubs).  No `-run` (no `mmap PROT_EXEC`); no floats (no x87 FPU init).  Cross-built by `build-tcc.sh`, called from `iso.sh`.  **Self-host milestones**: v0.8 → `tcc /src/userspace/calc.c -o /tmp/calc.elf` and `tcc /src/userspace/sh.c -o /tmp/sh.elf` both rebuild correct binaries in-OS (verified by `test_tcc_rebuild_calc` + `test_tcc_rebuild_sh`).  v0.9 → the **kernel** rebuilds with TCC: `./build-kernel-tcc.sh` (host) produces a valid Multiboot 2 ELF that QEMU boots end-to-end; the generated `/apps/rebuild-kernel.sh` runs the same recipe inside Makar (test phase `TEST_CMDLINE='test_mode test=rebuild-kernel'`, marker `REBUILD-KERNEL: ALL PASS` / `FAIL`).  See `docs/handoff-self-hosting.md`. |
| `sh.elf` | Ring-3 userspace shell (v0.8 + slices 20b/20c + POSIX A1-A3 in PR #181).  Freestanding (only `#include "syscall.h"`, no libc shim) so TCC can rebuild it in-OS.  Inline-edit readline driven byte-by-byte via `sys_getkey()` (KEY_ARROW_{UP,DOWN,LEFT,RIGHT} sentinels for cursor + history nav; backspace mid-line with tail-shift; Ctrl-C aborts line; Ctrl-D on empty line exits) + 16-entry ring-buffer history with dup-suppression.  Per-shell variable table (32 slots) with `NAME=value` assignment, `$VAR` / `${VAR}` / `$?` expansion across the whole line pre-tokenize, and `env` / `unset` / `read` / `wait` builtins (mirrors kernel `sh_script.c` + adds `wait` for the new `&` jobs table).  **POSIX additions (PR #181)**: `cmd1 \| cmd2 \| ...` pipelines via `SYS_PIPE`/`SYS_DUP2` (capped at 8 stages, per-stage redirect aware); `< file`, `> file`, `>> file`, `2> file`, `2>> file` redirection (extracted from argv before dispatch, applied via `open` + `dup2` in the forked child); top-level `&&` / `\|\|` / `&` list operators (gate the next segment on the previous segment's `$?`; `&` forks the segment, records the pid in a 16-slot `jobs[]`, and `wait` drains it).  Tokenize on whitespace + builtins (`cd` via SYS_CHDIR, `pwd` via SYS_GETCWD, `exit`, `env`, `unset`, `read`, `wait`) + external commands (`fork`+`execve`+`wait4`; argv[0] is either an absolute/relative path or a bareword applet name — bareword `ls`/`cat`/`cp`/`mv`/`rm`/`rmdir`/`echo`/`pwd` auto-routes to `/apps/makbox.elf <applet>` exactly like the kernel shell's restricted makbox fallback; `$?` reflects the child's `SYS_EXIT` low 7 bits).  Coexists with the in-kernel shell: `exec /apps/sh.elf` from any kernel shell drops into a `$ ` prompt; `exit` or Ctrl-D returns.  First concrete step toward the long-term goal of lifting the shell out of the kernel into userspace. |
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

Published docs: `docs/userland-libc.md` (freestanding libc + TCC path), `docs/posix.md` (what's POSIX-shaped + the gaps an app porter needs to know about), `SURVEY.md` (full inventory of shell commands / apps / VFS APIs / installer), `docs/internals.md` (deep-dive on kernel internals), `docs/kernel/` (per-subsystem pages).

## For agents new to the codebase

If you're an AI agent (Claude, Codex, etc.) picking this up cold:

1. **Read this file first** -- it's the canonical entry point.
2. **Then `CLAUDE.history.md` + `CLAUDE.roadmap.md`** -- shipped state + queued work.
3. **For VFS work**: this file's § VFS section explains the mount-table model; `src/kernel/arch/i386/fs/vfs.c` is the source of truth.  Slice 27 (the rootfs+overlay refactor) shipped recently -- no `resolve_rootfs_prefix` or path-rewriting tricks remain.
4. **For shell work**: there are TWO shells.  The in-kernel `shell.c` (~4.3 KLoc, default on every VT) and the ring-3 `src/userspace/sh.c` (freestanding, opt-in via `exec /apps/sh.elf`).  The userland shell is being lifted into parity over slices 20a–20f; both currently coexist.
5. **Conventions**: paths follow Linux (`/usr`, `/apps`, `/root`, `/proc`, `/dev`, `/mnt/<name>`, `/mnt/cdrom`).  The legacy `/mnt/hd` and bare `/hd` aliases were retired.  Apps live at `/apps/*.elf`, sources at `/src/`, headers at `/usr/include`, libc at `/usr/lib/libc.a`.
6. **Testing**: `./run.sh iso test` for kernel-side ktest + GDB checkpoints; `./run.sh ui [scenario]` for headless black-box scenarios; `./run.sh gui [scenario]` for visible-window debugging.  Add a scenario for any user-facing change you ship.
7. **Commits**: one commit per discrete work item; no `Co-Authored-By` trailers; no `Generated with Claude Code` footers.  Push to the existing PR branch when iterating.
8. **Build**: `./run.sh iso build` (Docker-wrapped cross-compile via `i686-elf-gcc`).  Clang diagnostics from your IDE will complain about missing kernel headers -- ignore them; the build uses the right include paths.
