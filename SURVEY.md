# Makar Shell & Userspace Survey

> Exhaustive inventory of what's wired up today. Sourced by reading the
> actual code, not aspirational. Last refreshed at v0.9.0 (kernel
> self-host milestone, May 2026).  For the freshest current-state
> summary see `CLAUDE.history.md`; for canonical project layout see
> `Directories.md`.

## Testing harness

Three test paths fan out from `./run.sh iso test`:

| Phase | Source | What it verifies |
|---|---|---|
| **ktest** | `src/kernel/arch/i386/proc/ktest.c` + `usertest.c` | In-kernel unit suites: PMM/paging invariants, ring-3 lifecycle, keyboard sentinel widths, VFS lookup, etc. Runs under `test_mode` cmdline; exits QEMU via isa-debug-exit. Output: `ktest.log` (with PASS/FAIL per assert). |
| **GDB ISO** | `tests/gdb_boot_test.py` | Boot-checkpoint + hardware-state probe under the QEMU GDB stub. Verifies Multiboot 2 magic, every `kernel_main` checkpoint, CR0.PG / CR3, PIT ticking, background ktest result, CD-ROM and HDD rootfs content. Output: `gdb-test.log`, `gdb-serial.log`. |
| **GDB HDD** | `tests/gdb_hdd_test.py` | Same shape as GDB ISO but boots from `makar-hdd-test.img` (no CD-ROM) to prove FAT32-from-MBR boot + auto-mount. |
| **Shell/app** | `src/userspace/{shell-smoke,incore,libc-tcc}.sh` | In-guest sh scripts run by `iso test` Phase 1; each command's `$?` gates a PASS/FAIL line, asserting on serial markers (`SHELL-SMOKE`/`INCORE`/`LIBC-TCC: ALL PASS`). Ring-3 shell behaviour driven via `sh.elf -c`. No host keyboard. |
| **Keyboard** | `keyboard_test_driver()` (`drivers/keyboard.c`), `./run.sh kbtest` | In-guest key injection (`keyboard_inject_text`/`keyboard_inject_key`) into the live decode→ring→shell pipeline; asserts on in-kernel state + serial `KBTEST:` markers. For paths where the keystroke itself is under test. |

All four phases run in parallel CI jobs (`.github/workflows/build-test.yml`).

## Shell Commands (Kernel Builtins)

### Filesystem Commands (`shell_cmd_fs.c`, `shell_cmd_fileops.c`)
- **mount** - `mount /dev/hdaN /mnt/<name>` binds a FAT32 or ext2 partition to an empty mountpoint under `/mnt` (mkdir `/mnt/<name>` first); the legacy numeric form and the `/mnt/hd` default were retired
- **umount** - `umount [/mnt/<name>]` unmounts the sole bound HD volume if unique; `umount /mnt/cdrom` unmounts and ejects the CD-ROM
- **ls** - List directory contents (supports VFS paths: `/`, `/apps`, `/usr`, `/mnt/cdrom/`, `/dev`, `/proc`)
- **cat** - Print file contents to terminal
- **cd** - Change current working directory
- **mkdir** - Create a directory (FAT32 only)
- **mkfs** - Format partition as FAT32
- **isols** - List ISO9660 directory (CD-ROM only)
- **write** - Create/overwrite file with text arguments
- **touch** - Create empty file
- **cp** - Copy file (uses 64 KiB staging buffer; `vfs_read_file` + `vfs_write_file`)
- **rm** - Delete a file (`vfs_delete_file`)
- **rmdir** - Delete an empty directory (`vfs_delete_dir`); errors if non-empty
- **mv** - Move or rename a file or directory (`vfs_rename`)

### Disk Commands (`shell_cmd_disk.c`, lines 104–402)
- **lsdisks** - List detected ATA/ATAPI drives with sizes
- **lspart** - List partitions on a drive (MBR/GPT aware)
- **mkpart** - Interactively create MBR or GPT partition tables
- **readsector** - Hex-dump a sector by LBA
- **chainload** - Load and execute a bootloader from a sector (never returns)

### System Commands (`shell_cmd_system.c`)
- **echo** - Print arguments to terminal
- **meminfo** - Show heap used/free bytes
- **uptime** - Humanised h/m/s + raw 100 Hz tick count
- **tasks** - List kernel tasks (state: ready/running/dead). See also `cat /proc/tasks`.
- **shutdown** - Power off via ACPI S5 (never returns)
- **reboot** - Reboot via ACPI (never returns)
- **panic** - Trigger kernel panic with optional message
- **ktest** - Run all in-kernel unit tests
- **verbose [on|off]** - Toggle `t_putchar` → COM1 mirror at runtime. Reports current state with no args.

### Application Commands (`shell_cmd_apps.c`, lines 26–192)
- **vix** - Launch VIX interactive text editor on a file
- **install** - Run OS installer from CD-ROM to HDD
- **exec** - Execute userspace ELF from a PATH directory (default `/apps/`; rootfs election routes to the live medium)
- **eject** - Eject HDD or CD-ROM (`umount /mnt/cdrom` also ejects)
- **ring3test** - Ring 3 test harness (defined in `proc/usertest.c`)

### Display Commands (`shell_cmd_display.c` - not fully read)
Commands exist but details TBD; likely: clear, fgcol, bgcol, setmode

### Manual/Help Commands (`shell_cmd_man.c`, `shell_help.c` - not fully read)
Help/manual pages; details TBD

## Command Dispatch (shell.c, lines 456–502)

The shell uses a **module table** pattern:
```c
static const shell_cmd_entry_t * const cmd_modules[] = {
    man_cmds,
    help_cmds,
    display_cmds,
    system_cmds,
    disk_cmds,
    fs_cmds,
    apps_cmds,
    NULL,
};
```

When a command is not found in built-ins, it falls back to **PATH lookup**:
- Iterates the colon-separated `PATH` shell variable (settable like any var; default `/apps`), trying `<dir>/<cmd>.elf`
- Calls `shell_exec_elf()` to spawn the app as a new kernel task

## Userspace Apps (ELF Executables)

All apps in `/Users/arawn/Makar/src/userspace/` compile to `.elf` files and are copied to `isodir/apps/` by `iso.sh` (line 5).

### **hello.elf** (hello.c)
- Interactive prompt: "What is your name?"
- Reads stdin, echoes "Hello, <name>!"
- Syscalls: `sys_write()`, `sys_read()`

### **diskinfo.elf** (diskinfo.c)
- Calls `sys_disk_info()` to get drive list
- Prints formatted drive information
- Syscalls: `sys_disk_info()`, `sys_write()`

### **ls.elf** (ls.c)
- Userspace equivalent of shell `ls` command
- Calls `sys_ls_dir()` to enumerate directories
- Takes optional path argument (default ".")
- Syscalls: `sys_ls_dir()`, `sys_write()`, `sys_exit()`

### **echo.elf** (echo.c)
- Userspace equivalent of shell `echo` command
- Prints arguments separated by spaces
- Syscalls: `sys_write()`

### **calc.elf** (calc.c, lines 1–147)
- Interactive REPL calculator with recursive-descent parser
- Grammar: `expr = term (('+' | '-') term)*`
- Supports: `*`, `/`, `%`, parentheses, unary `-`, `/`
- Commands: `quit` or `exit` to exit
- Syscalls: `sys_write()`, `sys_read()`

### **help.elf** (help.c, lines 1–50)
- Prints hardcoded list of built-in commands and their usage
- Groups: Display, System, Disk, Filesystem, Apps, Editor
- Syscalls: `sys_write()` only

### **vix.elf** (vix.c)
- VIX text editor (ring-3 port of kernel `proc/vix.c`; renamed from VICS in May 2026 — see `docs/makar-medli.md`)
- All kernel calls replaced with syscalls
- Features: line numbers, word wrap, flashing block caret, status bar, Ctrl+S save, Ctrl+Q quit
- Supports: 256 lines × 80 chars, 64 KiB file max
- Syscalls: `sys_putch_at()`, `sys_getkey()`, `sys_set_cursor()`, `sys_tty_clear()`, `sys_term_size()`, `sys_write_file()`, `sys_write()`, `sys_read()`

### **kbtester.elf** (kbtester.c)
- Visual press-all-keys keyboard diagnostic (ring-3)
- Renders a QWERTY layout, lights up cells on key make, logs every scancode/keycode/sentinel + modifier vector to serial via `sys_write_serial()`
- Exit: hold ESC for 4 s, or Ctrl+C (shell observes sigint and force-kills the child)
- Used by the keyboard-rewrite verification work (PR #124, slice 5b)

### **wget.elf** (wget.c)
- Ring-3 HTTP downloader; thin wrapper over the `SYS_WGET` syscall (255)
- `wget <http://host[:port]/path> [outfile]`; outfile defaults to `/tmp/<basename>`
- Plain HTTP only (no TLS). Also available as an in-kernel shell builtin (`shell_cmd_net.c`)
- Pairs with the `unzip` shell command (`fs/unzip.c` + `fs/inflate.c`) to fetch + extract archives

### **gui.elf** (wm.c) — the makx display server
- X11-style **display server**: owns the framebuffer + keyboard + mouse, draws the desktop chrome (window borders, dock, menu bar, cursor) and composites client windows. The sole `SYS_FB_PRESENT` caller.
- Applications are separate **client** processes over IPC (control) + shared surfaces (pixels); protocol + client lib in `makx.{h,c}`. Server launches clients with `-makx <pid>`.
- `gui.elf uitest` / `gui.elf fstest` are headless self-tests (`GUI-UITEST` / `GUI-FSTEST` markers, run by `shell-smoke.sh`). Built-in graphical login (`gui login`). See `docs/gui.md`.

### makx clients (`mxterm.elf`, `mxfiles.elf`, `mxedit.elf`, `mxtasks.elf`, `doom.elf`)
- **mxterm** (mxterm.c) — terminal: forks `sh.elf` over pipes, renders the byte grid into its surface.
- **mxfiles** (mxfiles.c) — file browser on the shared `gui_browser` model (Up/Open/Refresh/Go).
- **mxedit** (mxedit.c) — multi-line text editor with the shared open/save dialog (`br_dialog`).
- **mxtasks** (mxtasks.c) — task manager: `/proc/tasks` list, Kill, refresh-interval slider.
- **doom.elf** (doomgeneric_makar.c) — windowed makx client with `-makx`; unchanged fullscreen path without it.
- Shared client libs: `gui_gfx` (drawing), `gui_ui` (immediate-mode widgets), `gui_browser` (FS model + file dialog), `makx` (protocol client).

## Userspace Syscall API (`src/userspace/syscall.h`)

### POSIX-Compatible I/O
- `sys_read(fd, buf, len)` → `SYS_READ (3)`
- `sys_write(fd, buf, len)` → `SYS_WRITE (4)`
- `sys_open(path, flags)` → `SYS_OPEN (5)`
- `sys_close(fd)` → `SYS_CLOSE (6)`
- `sys_lseek(fd, offset, whence)` → `SYS_LSEEK (19)`
- `sys_brk(addr)` → `SYS_BRK (45)`

### Task Control
- `sys_exit(status)` → `SYS_EXIT (1)`
- `sys_yield()` → `SYS_YIELD (158)`

### Terminal I/O
- `sys_getkey()` → `SYS_GETKEY (200)` - raw keyboard; returns arrow sentinels 0x80–0x83
- `sys_putch_at(cells, n)` → `SYS_PUTCH_AT (201)` - write n screen cells
- `sys_set_cursor(col, row)` → `SYS_SET_CURSOR (202)`
- `sys_tty_clear(clr)` → `SYS_TTY_CLEAR (203)` - fill with VGA colour
- `sys_term_size()` → `SYS_TERM_SIZE (204)` - returns `(cols << 16) | rows`

### Filesystem & VFS
- `sys_write_file(path, buf, len)` → `SYS_WRITE_FILE (205)` - create/overwrite file
- `sys_ls_dir(path, buf, bufsz)` → `SYS_LS_DIR (206)` - enumerate directory
- `sys_disk_info(buf, bufsz)` → `SYS_DISK_INFO (207)` - list drives
- `sys_delete_file(path)` → `SYS_DELETE_FILE (208)`
- `sys_rename_file(old, new)` → `SYS_RENAME_FILE (209)` (also handles directory rename)
- `sys_delete_dir(path)` → `SYS_DELETE_DIR (210)` (errors if non-empty)

### Makar extensions (211–214)
- `sys_write_serial(buf, len)` → `SYS_WRITE_SERIAL (211)` - COM1-only write; no framebuffer mirror. Used by `kbtester.elf`.
- `sys_keyboard_raw(enable)` → `SYS_KEYBOARD_RAW (212)` - enable/disable raw keyboard mode (1 = raw bytes, no sentinel translation)
- `sys_shell_clear()` → `SYS_SHELL_CLEAR (213)` - same as the shell's `clear` builtin
- `sys_uptime()` → `SYS_UPTIME (214)` - returns the 100 Hz PIT tick counter

## VFS API (`src/kernel/include/kernel/vfs.h`)

**Unified namespace:**
- `/` - virtual root; rootfs election elevates a disk volume here so `/usr`, `/etc`, `/home`, `/apps`, `/root` resolve via the rootfs.  `vfs_complete()` enumerates entries so `cd /<TAB>`, `cat /*`, `ls /p*` all work
- `/mnt` - user-mountable container; lists empty mountpoints (`[boot]`, `[root]`) + any user mkdir'd ones; mounts elevated to `/` or `/boot` are hidden
- `/mnt/cdrom/…` - ISO9660 CD-ROM (auto-detected at init, elevated to `/` on live boot)
- `/proc/…` - synthetic, always-present read-only view of kernel state. Backed by `arch/i386/fs/procfs.c`; mount path is the `PROCFS_MOUNT` constant in `include/kernel/procfs.h`. Entries: `cpuinfo`, `meminfo`, `tasks`, `uname`, `rtc` (content generated on each read; no caching)
- `/dev/…` - synthetic block-device tree (`arch/i386/fs/devfs.c`): `/dev/hda[N]` ATA disks/partitions, `/dev/cdrom` ATAPI. Byte-addressed read/write via `devfs_pread`/`devfs_pwrite` over native sector I/O; opened as `FD_KIND_BLOCKDEV`

### Lifecycle
- `vfs_init()` - probe ATAPI for ISO9660 CD-ROM; register synthetic overlays (`/dev`, `/proc`, `/tmp`, `/log`) and `/mnt/cdrom` (when present) as first-class mount-table entries; pre-register empty `/mnt/boot` + `/mnt/root` placeholders; build the `/dev` node table
- `vfs_set_boot_drive(biosdev)` - record BIOS boot device
- `vfs_mount_root(spec)` - elect and bind the rootfs at `/`.  Spec resolution: `/dev/hdaN` (explicit), `"none"` (skip), NULL/auto (walk every ATA partition probing for `/usr/lib/crt0.o`; CD-ROM fallback for live boots)
- `vfs_auto_mount()` - probe ATA drives; single-partition disks bind at `/mnt/root`, dual-partition installer layouts bind partition 0 at `/mnt/boot` (also mirrored at `/boot`) + partition 1 at `/mnt/root`
- `vfs_ensure_root_home()` - best-effort `mkdir /root` on writable rootfs (ext2/FAT32) boots; no-op on ISO9660 or no-rootfs
- `vfs_mount_hd(drive, lba, name)` - bind a HD volume into an existing empty mountpoint (`mkdir /mnt/<name>` first); legacy `/mnt/hd` default removed
- `vfs_prepare_shutdown()` - flush + unmount before power-off/reset (called from `shutdown`/`reboot`)
- `vfs_notify_cdrom_ejected()` - called after ATAPI eject; drops every mount-table entry pointing at the CD-ROM (typically `/mnt/cdrom`, plus `/` if it was elected as rootfs)

### Operations
- `vfs_ls(path)` - list directory
- `vfs_cd(path)` - change CWD
- `vfs_cat(path)` - print file contents
- `vfs_mkdir(path)` - create directory (FAT32 only)
- `vfs_read_file(path, buf, bufsz, out_sz)` - read file
- `vfs_write_file(path, buf, size)` - create/overwrite file (FAT32 only)
- `vfs_file_exists(path)` - check if readable
- `vfs_complete(dir, prefix, cb, ctx)` - enumerate for tab completion
- `vfs_getcwd()` - return current directory string

## FAT32 API (`src/kernel/include/kernel/fat32.h`)

**One mounted volume at a time.** Supports 8.3 + LFN (Long File Names).

### Mount/Unmount
- `fat32_mount(drive, part_lba)` → 0 on success, -1 (I/O) or -2 (not FAT32)
- `fat32_unmount()` - unmount current
- `fat32_mounted()` - return 1 if mounted, 0 otherwise

### Directory Operations
- `fat32_ls(path)` - list to terminal
- `fat32_cd(path)` - change CWD
- `fat32_getcwd()` - return CWD string
- `fat32_mkdir(path)` - create directory

### File I/O
- `fat32_read_file(path, buf, bufsz, out_sz)` → 0 success, -ve error
- `fat32_write_file(path, buf, size)` → 0 success, -ve error (creates or overwrites)
- `fat32_file_exists(path)` → 1 if exists, 0 otherwise

### Format
- `fat32_mkfs(drive, part_lba, part_sectors)` → 0 success
  - Returns -1 (bad args), -2 (I/O), -6 (partition too small; need ≥ 32 MiB)

### Tab Completion
- `fat32_complete(dir_path, prefix, cb, ctx)` - callback per entry

## Installer (`src/kernel/arch/i386/proc/installer.c`, lines 137–466)

Fully interactive OS installer. Steps:

1. **Probe ISO9660** (lines 146–167) - Find ATAPI CD-ROM with ISO
2. **List ATA drives** (lines 172–192) - Prompt user to select target HDD
3. **Confirm destructive write** (lines 216–231) - Type "yes" to proceed
4. **Install GRUB bootloader** (lines 242–352):
   - Read `boot.img` and `core.img` from ISO
   - Patch `boot.img` with `core.img` LBA (sector 1)
   - Build MBR at sector 0 (GRUB code + partition table + 0x55AA signature)
   - Write `core.img` to sectors 1..N (embedding area before first partition)
5. **Format FAT32** (lines 357–364) - `fat32_mkfs()` at LBA 2048
6. **Mount volume** (lines 369–376) - `fat32_mount()` for directory creation
7. **Create directory tree** (lines 381–384):
   - `/boot`
   - `/boot/grub`
   - `/boot/grub/i386-pc`
8. **Copy kernel** (lines 389–394) - `/boot/makar.kernel` from ISO
9. **Copy GRUB modules** (lines 399–428):
   - `normal.mod`, `part_msdos.mod`, `fat.mod`, `multiboot2.mod`, `linux.mod`
   - Non-fatal if missing
10. **Write grub.cfg** (lines 433–455):
    - Sets `timeout=0` (no user prompt; boots immediately)
    - Multiboot2 path: `/boot/makar.kernel`
    - Includes explicit `boot` command
11. **Unmount & success** (lines 460–465) - Report success; user removes CD-ROM and reboots

## iso.sh Script (`iso.sh`, lines 1–79)

Builds bootable ISO. Steps:

1. Create `isodir/` structure with subdirs: `boot/grub/i386-pc`, `apps`, `src`, `docs`
2. Copy kernel: `sysroot/boot/makar.kernel` → `isodir/boot/makar.kernel`
3. **Copy source tree**: `src/. → isodir/src/` (readable via VIX on ISO)
4. **Copy docs**: `docs/. → isodir/docs/`
5. Write GRUB config: `isodir/boot/grub/grub.cfg`
   - Sets `timeout=5` (5-second user prompt on CD-ROM boot)
   - Menuentry "Makar OS" loads kernel
   - Menuentry "Next available device" exits to firmware
6. Locate host GRUB i386-pc directory (`/usr/lib/grub/i386-pc` or `/usr/share/grub/i386-pc`)
7. Copy GRUB files to ISO:
   - `boot.img` (needed by installer to patch and embed)
   - Generate `core.img` via `grub-mkimage` with embedded search config
   - Copy modules: `normal.mod`, `part_msdos.mod`, `fat.mod`, `multiboot2.mod`, `linux.mod`
8. Build ISO: `grub-mkrescue -o makar.iso isodir`

**Apps copied to `isodir/apps/`:** All `.elf` files from `src/userspace/` at build time (implied by installer and shell PATH lookup).

---

## Summary Table

| Category | Count | Status |
|---|---|---|
| Shell filesystem commands | 14 | ✅ Complete (mount, umount, ls, cat, cd, mkdir, mkfs, isols, write, touch, cp, rm, rmdir, mv) |
| Shell disk commands | 5 | ✅ Complete (lsdisks, lspart, mkpart, readsector, chainload) |
| Shell system commands | 8 | ✅ Complete (echo, meminfo, uptime, tasks, shutdown, reboot, panic, ktest) |
| Shell app commands | 5 | ✅ Complete (vix, install, exec, eject, ring3test) |
| Userspace ELF apps | 10 | ✅ Complete (hello, diskinfo, ls, echo, calc, help, vix, rm, mv, cp) |
| VFS operations | 13 | ✅ Complete (init, mount, ls, cd, cat, mkdir, read, write, exists, complete, delete_file, delete_dir, rename) |
| FAT32 read/write APIs | 12 | ✅ Complete (mount, umount, ls, cd, mkdir, read, write, mkfs, delete_file, delete_dir, rename_file, rename_dir) |
| Userspace syscalls | 14 | ✅ Complete I/O + 6 Makar extensions (write_file, ls_dir, disk_info, delete_file, rename_file, delete_dir) |

