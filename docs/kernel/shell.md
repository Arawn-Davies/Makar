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
and Tab completion (first token: command names; subsequent tokens: VFS paths
via `vfs_complete()`).

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

If no built-in matches, the shell searches `/cdrom/apps/` then `/hd/apps/`
for a matching ELF and runs it via `elf_exec`.

---

## Built-in commands

### Filesystem (`shell_cmd_fs.c`, `shell_cmd_fileops.c`)

| Command | Description |
|---|---|
| `ls [path]` | List directory; supports `/hd/` and `/cdrom/` paths |
| `cd <path>` | Change VFS working directory |
| `cat <path>` | Print file contents |
| `mkdir <path>` | Create directory (FAT32 only) |
| `mount <drive> <part>` | Mount FAT32 partition to `/hd/` |
| `umount` | Unmount the current FAT32 volume |
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
| `uptime` | Ticks since boot |
| `tasks` | List kernel tasks and their states |
| `shutdown` | ACPI S5 power-off |
| `reboot` | ACPI reboot |
| `panic [msg]` | Trigger kernel panic |
| `ktest` | Run all in-kernel unit tests interactively |

### Application (`shell_cmd_apps.c`)

| Command | Description |
|---|---|
| `exec <path>` | Load and run a userspace ELF (Ctrl+C kills it) |
| `vics <path>` | Launch VICS text editor |
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
