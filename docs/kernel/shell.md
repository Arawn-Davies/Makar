---
title: shell
parent: Kernel subsystems
grand_parent: Reference
---

# shell - Interactive kernel command shell

**Header:** `kernel/include/kernel/shell.h`  
**Sources:** `kernel/arch/i386/shell/shell.c`, `shell_cmd_*.c`, `shell_help.c`

Provides an interactive read-eval-print loop (REPL) running as a cooperative
kernel task (`shell_run`).  Four independent shell instances run concurrently
on TTYs 1–4 (Alt+F1–F4 switches focus).

---

## How it works

`shell_run` loops forever, printing a prompt, reading a line of input,
splitting it into tokens, and dispatching to a command handler.

### Input (`shell_readline`)

Handles inline editing: cursor movement, insert-at-point, Backspace, Enter,
Ctrl+C (aborts line, prints `^C`), history navigation (↑/↓ up to 16 entries),
and Tab completion. First token completes against the union of built-in
command names and `*.elf` basenames found in `s_app_path`. Subsequent
tokens complete VFS paths via `vfs_complete()` - cross-filesystem, so
`cd /<TAB>` enumerates the root (`mnt`, `proc`, `dev`, `usr`, `apps`,
`root`, ...), `cd /mnt/<TAB>` lists the live disk mounts (`boot`, `root`,
`cdrom`), `cat /proc/c<TAB>` matches `cpuinfo`, and `ls /apps/<TAB>` walks
the rootfs apps directory. Globbing
(`*`, `?`) on argv is expanded via `shell_glob.c`
before dispatch using the same `vfs_complete()` enumerator.

### Parsing

Splits the input in-place on spaces into up to 8 `argv`-style tokens.  Empty
lines are skipped.

### Dispatch

Commands are looked up in a **module table** - a NULL-terminated array of
`shell_cmd_entry_t[]` pointers, one per category file:

```c
static const shell_cmd_entry_t * const cmd_modules[] = {
    man_cmds, help_cmds, display_cmds, system_cmds,
    disk_cmds, fs_cmds, apps_cmds, fileops_cmds, NULL,
};
```

Each entry is `{ name, fn, fullscreen }`. The `fullscreen` bit marks
handlers that paint directly to the framebuffer (install, exec) -
after such a handler returns, `shell_dispatch` calls
`shell_restore_screen()` which repaints the focused VT's backing grid
plus the status bar. That puts the shell's history back without
waiting for the next keystroke and removes the need for each
"fullscreen" command to clean up after itself.

If no built-in matches, the shell tries `try_exec_path()` on the
literal argv[0] (if it's a path-style `/abs` or `./rel`), then walks
the **PATH** directories appending `[.elf]`. PATH is the per-task
`PATH` shell variable (settable like any var) and falls back to the
built-in default `/apps`; both command dispatch
and first-token tab completion iterate it via `shell_path_dir()`.
Successful ELF execution is also followed by `shell_restore_screen()` -
any ring-3 binary is treated as potentially-fullscreen. `vix` is no
longer a builtin: it resolves through PATH to `vix.elf` and runs as its
own ring-3 task (so it shows up in `maktop`).

---

## Built-in commands

### Filesystem (`shell_cmd_fs.c`, `shell_cmd_fileops.c`)

| Command | Description |
|---|---|
| `ls [path]` | List directory; supports `/`, `/apps`, `/usr`, `/mnt/cdrom/`, `/dev`, `/proc` |
| `cd <path>` | Change VFS working directory |
| `cat <path>` | Print file contents |
| `mkdir <path>` | Create directory (FAT32 only); `mkdir /mnt/<name>` creates an empty mountpoint |
| `mount /dev/hdaN /mnt/<name>` | Bind a FAT32 or ext2 partition to an empty mountpoint (mkdir `/mnt/<name>` first; the legacy numeric form and `/mnt/hd` default were retired) |
| `umount [/mnt/<name>]` | Unmount the sole bound HD volume if unique; `umount /mnt/cdrom` unmounts + ejects the CD-ROM |
| `mkfs <drive> <part>` | Format partition as FAT32 |
| `isols [path]` | List ISO9660 directory (CD-ROM) |
| `write <path> <text…>` | Create/overwrite file with text arguments |
| `touch <path>` | Create empty file |
| `cp <src> <dst>` | Copy file (reads via VFS, writes via FAT32) |
| `rm <path>` | Delete a file |
| `rmdir <path>` | Delete an empty directory |
| `mv <src> <dst>` | Move or rename a file or directory |

### Disk (`shell_cmd_disk.c`)

| Command | Description |
|---|---|
| `lsdisks` | List detected ATA/ATAPI drives |
| `lspart <drive>` | List MBR/GPT partitions |
| `mkpart <drive>` | Interactively create partition table |
| `readsector <drive> <lba>` | Hex-dump a sector |
| `chainload <drive> <lba>` | Load and execute a boot sector |

### System (`shell_cmd_system.c`)

| Command | Description |
|---|---|
| `echo [args…]` | Print arguments to terminal |
| `meminfo` | Heap used/free in bytes |
| `uptime` | Humanised h/m/s + raw 100 Hz tick count |
| `tasks` | List kernel tasks and their states (`cat /proc/tasks` is the richer variant) |
| `shutdown` | Flush + unmount the FAT32 volume, then ACPI S5 power-off |
| `reboot` | Flush + unmount the FAT32 volume, then ACPI reboot |
| `panic [msg]` | Trigger kernel panic |
| `ktest` | Run all in-kernel unit tests interactively |
| `verbose [on\|off]` | Toggle the `t_putchar` → COM1 mirror at runtime. Equivalent to flipping `console=ttyS0` on the kernel cmdline. Used by the in-guest test drivers to grep shell output from serial. |

### Application (`shell_cmd_apps.c`)

| Command | Description |
|---|---|
| `exec <path>` | Load and run a userspace ELF (Ctrl+C kills it) |
| `install` | Run OS installer from CD-ROM to HDD |
| `eject` | Eject HDD or CD-ROM |
| `ring3test` | Ring-3 test harness |

### Display (`shell_cmd_display.c`)

| Command | Description |
|---|---|
| `clear` | Clear the screen |
| `setmode <WxH>` | Switch VESA resolution at runtime |
| `fgcol` / `bgcol` | Set foreground/background colour |

### Manual (`shell_cmd_man.c`, `shell_help.c`)

| Command | Description |
|---|---|
| `lsman` | List all commands with one-line descriptions |
| `man <cmd>` | Show the manual page for a command |

---

## Functions

### `shell_run`

```c
void shell_run(void);
```

Enter the interactive shell loop.  Never returns.  Called as the entry point
of each TTY task.

### `shell_readline`

```c
void shell_readline(char *buf, size_t size);
```

Read one line of input into `buf` with full inline editing, history, and Tab
completion.  Used by both the shell REPL and `SYS_READ` for stdin.
