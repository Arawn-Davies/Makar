# Makar current state, changelog & attribution (Claude reference)

Companion to `CLAUDE.md`. Snapshot of subsystem state, recently-merged PRs, and FOSS
attribution. Consult for "what's already shipped" / "what does subsystem X do today"
context; not needed for routine edits.

## Current state (as of May 2026)

Makar boots to an interactive VESA shell with 4 independent TTYs.
Alt+F1–F4 switches between them; each is a separate **preemptive** kernel
task with its own kernel stack, its own backing screen buffer (so a
background TTY's accumulated output survives a focus switch — Linux VT
behaviour), and (for ring-3 programs) its own page directory. Major
subsystems:

- **Display**: VESA framebuffer (Bochs VBE, defaults to 720p), VGA text fallback (80×50). Pane abstraction (`vesa_pane_t`) used by VIX. Per-TTY logical character grid (`vt_buf_t` in `display/vt.c`) backs every shell — writes go to the grid first; the framebuffer is only painted when that TTY is focused. After any "fullscreen" shell command returns (vix, install, any ELF launched via `exec` or PATH), `shell_dispatch` calls `shell_restore_screen()` which repaints the focused VT's grid to the FB — so post-exit screen is never blank.
- **Multi-TTY**: 4 shell tasks (`shell0`–`shell3`). `vtty.c` routes keyboard input via `task_t.tty` (authoritative) and tracks the focused slot. `vtty_switch()` defers the framebuffer repaint out of IRQ context to `vtty_drain_pending()`, which runs from the destination shell's `keyboard_getchar` poll loop. A tmux-style status bar (the multiplexer is named "makmux") lives in the reserved bottom row: a left label, the centred `VT1 VT2 VT3 VT4` indicators (1-based, active slot highlighted), and an `Alt+F1-F4` hint. Alt+F5 toggles the left label between `Makar` and a live RTC clock (`HH:MM:SS DD/MM/YY`). The timer IRQ drives the spinner + clock via a per-tick hook registry (`timer_register_tick_hook`) instead of calling the display layer directly.
- **VIX**: vi-style text editor, now a **userland** ELF (`vix.elf`, the `vix` command) running as its own ring-3 task — so it appears in maktop with its own pid + memory.  Vim-style line-number gutter, word wrap, `~` EOF rows, and a flashing block caret (`SYS_CARET_STYLE`); derives geometry from `SYS_TERM_SIZE` so it works at any resolution. Modelled on ELKS/FUZIX vi. The earlier in-kernel implementation (`proc/vix.c`) was removed once the userland port reached feature parity.
- **Storage**: FAT32 + ext2 (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. VFS layer with CWD, auto-mount. Full read/write/delete/rename/mkdir on both FAT32 and ext2; `mkfs.fat32` / `mkfs.ext2` format. Disk filesystems live under `/mnt` (Linux convention): FAT32 auto-mounts at `/mnt/hd` (kernel + bootloader modules + root, EFI-partition style) and the CD-ROM at `/mnt/cdrom`. `mount /dev/hdaN /mnt/<name>` auto-detects the backend by superblock (ext2 else FAT32); the VFS keeps a **mount table** (`s_mounts[]` in `vfs.c`) so one FAT32 + one ext2 volume coexist at separate mountpoints — the drivers are single-volume, so at most one of each. `vfs_mount_hd`/`vfs_umount_hd`/`vfs_hd_fsname` manage it; each `VFS_FS_HD` op dispatches per path through `hd_*` wrappers carrying the backend id. The ext2 driver (`fs/ext2.c`) supports rev0/rev1, 1K/2K/4K blocks, FILETYPE incompat only (refuses journal/extents/etc.), reads via direct + single/double-indirect maps, and writes/mkdir/delete/rename via block + inode bitmap allocation. (The earlier bare `/hd` / `/cdrom` aliases were removed — `/mnt/...` is the only form.) Synthetic `/proc` mount exposes `cpuinfo`, `meminfo`, `tasks`, `uname`, `rtc` as read-only files generated on demand.
- **Block devices (`/dev`)**: Synthetic devfs mount exposing raw IDE storage as byte-addressed nodes: `/dev/hda`, `/dev/hdb`… (whole ATA disks), `/dev/hdaN` (their MBR/GPT partitions, as offset windows), `/dev/cdrom` (ATAPI, read-only). Reads/writes translate to native sector I/O (512 B ATA / 2048 B ATAPI) with internal read-modify-write so callers can transfer arbitrary offset/length. Opening a `/dev` node binds the fd as `FD_KIND_BLOCKDEV` (no eager buffer); `SYS_READ`/`SYS_WRITE`/`SYS_LSEEK` route through `devfs_pread`/`devfs_pwrite`. Backs `fdisk.elf` and `mount /dev/hdaN /mnt/<name>`.
- **Tasking**: Round-robin scheduler with timer-driven preemption (PIT 100 Hz, `SCHED_QUANTUM = 4` ticks → 40 ms slice). Per-task `pid`, `cwd`, `tty`, signal bitmasks, and real per-task fd table (`fd_table_t` in `kernel/fd.h`, 16 slots, fds 0/1/2 pre-bound to stdin/stdout/stderr). User PD reaped on task exit, fd table reaped on slot reuse. Background ktest harness runs before the shell prompt appears.
- **Userspace**: Ring-3 protected mode via `iret`. ELF loader (`elf_exec`) with argc/argv. Syscalls: `SYS_EXIT`, `SYS_READ`, `SYS_WRITE` (fd 1 = VGA, fd 2 = VGA + COM1 serial), `SYS_OPEN`, `SYS_CLOSE`, `SYS_LSEEK`, `SYS_BRK`, `SYS_DEBUG`, `SYS_YIELD`, plus Makar extensions (200–218 - terminal/file ops + `SYS_WRITE_SERIAL` + `SYS_GETCWD` + `SYS_CARET_STYLE`). Apps: `calc.elf`, `hello.elf`, `makbox.elf` (ls/cat/cp/mv/rm/rmdir/echo/pwd multicall), `vix.elf`, `diskinfo.elf`, `fdisk.elf`, `basic.elf` (integer BASIC w/ graphics), `clock.elf`, `maktop.elf`, `kbtester.elf`.
- **Shell**: Inline editing, history, tab completion, Ctrl+C sigint. `lsman` / `man <cmd>` replace `help`. Built-in file ops: `rm`, `rmdir`, `mv`. `uptime` shows humanised h/m/s. `cat /proc/<entry>` for system introspection. `PATH` is a settable shell variable (default `/mnt/cdrom/apps:/mnt/hd/apps`) consulted by command dispatch + tab completion. `mount /dev/hdaN /mnt/<name>` mounts a FAT32 partition at a chosen mountpoint (legacy `mount <drv> <part#>` → `/mnt/hd`); `umount [/mnt/<name>]` flushes + unmounts the FAT32 volume, `umount /mnt/cdrom` ejects the CD. `shutdown`/`reboot` flush + unmount first.
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
| (pending) | `feat/devfs-fdisk` | Synthetic `/dev` block devices (devfs, `FD_KIND_BLOCKDEV`), `fdisk.elf` MBR editor, ATAPI READ CAPACITY for `/dev/cdrom` sizing; disk filesystems moved under `/mnt` (`/mnt/hd`, `/mnt/cdrom`, `/hd`+`/cdrom` aliases); `mount /dev/hdaN /mnt/<name>`; `umount /mnt/cdrom` eject; flush+unmount on shutdown/reboot; settable `PATH` shell var; `/proc/meminfo MemUsed` folds in heap; QEMU `-m 32` everywhere; ktest `test_devfs` + ui scenarios `ls-dev`/`ls-mnt` |

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
