/*
 * shell_cmd_man.c - man page viewer and command lister.
 *
 * lsman : list all built-in commands in a fixed-width table.
 * man   : print the full manual entry for a named command.
 *
 * Man text is static - no filesystem dependency.
 */

#include "shell_priv.h"
#include <kernel/tty.h>

/* -------------------------------------------------------------------------
 * Man page table
 * Fields: name, brief (one line, no name prefix), body (full text)
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *brief;
    const char *body;
} man_entry_t;

#define COL 14   /* name column width (padded with spaces) */

static const man_entry_t man_table[] = {
    { "help",
      "list all built-in commands (alias for lsman)",
      "Usage: help\n\nAlias for lsman - prints the full command table.\n"
    },
    { "lsman",
      "list all built-in commands",
      "Usage: lsman\n\n"
      "Prints a table of every built-in shell command with a one-line description.\n"
      "Run 'man <cmd>' for full details on any entry.\n"
    },
    { "man",
      "display manual entry for a command",
      "Usage: man <cmd>\n\n"
      "Prints the manual page for the named built-in command.\n"
    },
    { "version",
      "show build info and copyright",
      "Usage: version\n\nPrints the kernel version, build date, and copyright notice.\n"
    },
    { "clear",
      "clear the screen",
      "Usage: clear\n\nClears the VESA/VGA display and resets the cursor to (0,0).\n"
    },
    { "setmode",
      "set VESA display resolution",
      "Usage: setmode <WxH>\n\n"
      "Valid resolutions: 320x240  640x480  1280x720  1920x1080\n\n"
      "Switches the Bochs VBE framebuffer to the requested resolution and\n"
      "re-initialises the VESA TTY.  VIX uses the new geometry automatically.\n"
    },
    { "fgcol",
      "set foreground colour (hex RRGGBB)",
      "Usage: fgcol RRGGBB\n\nExample: fgcol FF8800\n"
    },
    { "bgcol",
      "set background colour (hex RRGGBB)",
      "Usage: bgcol RRGGBB\n\nExample: bgcol 000088\n"
    },
    { "echo",
      "print arguments to the terminal",
      "Usage: echo [args...]\n\nPrints each argument separated by a space, followed by a newline.\n"
    },
    { "meminfo",
      "print heap used/free",
      "Usage: meminfo\n\nShows current heap allocation statistics.\n"
    },
    { "uptime",
      "print ticks since boot",
      "Usage: uptime\n\nPrints the time since boot in h:m:s.cc form, plus the raw PIT tick counter (100 Hz).\n"
    },
    { "tasks",
      "list kernel tasks",
      "Usage: tasks\n\nLists all tasks in the cooperative scheduler pool with their state.\n"
    },
    { "reboot",
      "reboot the system",
      "Usage: reboot\n\nTriggers a system reboot via the keyboard controller reset line.\n"
    },
    { "shutdown",
      "power off via ACPI S5",
      "Usage: shutdown\n\nInitiates an ACPI S5 soft power-off.\n"
    },
    { "ktest",
      "run in-kernel test suite",
      "Usage: ktest\n\n"
      "Runs all ktest suites and prints pass/fail to the terminal and serial.\n"
      "A silent background run also executes at boot before the shell starts.\n"
    },
    { "lsdisks",
      "list detected ATA drives",
      "Usage: lsdisks\n\nPrints capacity and model for all detected IDE drives.\n"
    },
    { "lspart",
      "list partitions on a drive",
      "Usage: lspart <drv>\n\nPrints the MBR or GPT partition table for the given drive number.\n"
    },
    { "readsector",
      "hex-dump one disk sector",
      "Usage: readsector <drv> <lba>\n\nReads and hex-dumps the 512-byte sector at the given LBA.\n"
    },
    { "mkfs",
      "format a partition as FAT32",
      "Usage: mkfs <drv> <part#>\n\n"
      "Formats the specified partition as FAT32.  DESTRUCTIVE - all data lost.\n"
    },
    { "mount",
      "mount a FAT32 partition at /hd/",
      "Usage: mount <drv> <part#>\n\nMounts the FAT32 partition at the /hd/ VFS mount point.\n"
    },
    { "umount",
      "unmount the FAT32 volume",
      "Usage: umount\n\nFlushes and unmounts the /hd/ FAT32 volume.\n"
    },
    { "ls",
      "list directory contents",
      "Usage: ls [path]\n\n"
      "Lists files and subdirectories.  Works on both /hd/ (FAT32)\n"
      "and /cdrom/ (ISO 9660).\n"
    },
    { "cd",
      "change working directory",
      "Usage: cd <path>\n\nChanges the shell CWD.  Use /hd/ for the HDD, /cdrom/ for the CD.\n"
    },
    { "cat",
      "print file contents to terminal",
      "Usage: cat <file>\n\nReads and prints a VFS file.  Works on both FAT32 and ISO 9660.\n"
    },
    { "pwd",
      "print current working directory",
      "Usage: pwd\n\nPrints the current VFS working directory path.\n"
    },
    { "write",
      "create/overwrite a file with text",
      "Usage: write <file> <text...>\n\n"
      "Writes all remaining arguments as a single line to the file.\n"
      "Requires FAT32 volume mounted at /hd/.\n"
    },
    { "touch",
      "create an empty file",
      "Usage: touch <file>\n\nCreates an empty file on the FAT32 volume.\n"
    },
    { "cp",
      "copy a file",
      "Usage: cp <src> <dst>\n\nCopies a file within the VFS.  Destination must be on FAT32.\n"
    },
    { "mkdir",
      "create a directory",
      "Usage: mkdir <path>\n\nCreates a directory on the FAT32 volume.\n"
    },
    { "vix",
      "open a file in the VIX editor",
      "Usage: vix <file>\n\n"
      "Opens the file in VIX (Visual Interactive Character Shell), a lightweight\n"
      "full-screen editor modelled on ELKS/FUZIX vi.  Adapts to the current VESA\n"
      "resolution automatically via the pane abstraction.\n\n"
      "Key bindings:\n"
      "  Arrow keys   navigate\n"
      "  Printable    insert at cursor\n"
      "  Backspace    delete; join lines at col 0\n"
      "  Enter        split line at cursor\n"
      "  Tab          insert 4 spaces\n"
      "  Ctrl+S       save (requires FAT32 volume at /hd/)\n"
      "  Ctrl+Q       quit (press twice to discard unsaved changes)\n\n"
      "Note: files on /cdrom/ are read-only (ISO 9660).  Save will fail with an\n"
      "error if the file path does not resolve to the FAT32 /hd/ volume.\n"
    },
    { "kbtester",
      "press-all-keys keyboard tester (ring-3 ELF)",
      "Usage: exec /apps/kbtester.elf\n\n"
      "Reads cooked bytes from the keyboard one at a time and prints, for each:\n"
      "the running count, hex value, decimal value, symbolic name (KEY_ARROW_UP,\n"
      "Ctrl-G, ESC, printable, etc.) and an echoed glyph or '<ctrl>' tag.\n\n"
      "Every line is mirrored to COM1 serial via SYS_WRITE_SERIAL so the byte\n"
      "stream can be captured off-host (qemu -serial stdio, real UART) and\n"
      "compared against expected scancodes -- the same way an online keyboard\n"
      "tester verifies a physical keyboard.\n\n"
      "Exit: Ctrl+C, or 'q'/'Q' typed on its own.\n"
    },
    { "exec",
      "load and run an ELF binary",
      "Usage: exec <path> [args...]\n\n"
      "Loads a freestanding ELF32 binary from the VFS and runs it in ring-3.\n"
      "Ctrl+C during execution force-kills the child task.\n\n"
      "Syscalls available to user programs (Linux i386 ABI, int 0x80):\n"
      "  EAX=1   SYS_EXIT\n"
      "  EAX=3   SYS_READ   EBX=0 (stdin), ECX=buf, EDX=count\n"
      "  EAX=4   SYS_WRITE  EBX=string ptr\n"
      "  EAX=100 SYS_DEBUG  EBX=checkpoint value\n"
      "  EAX=158 SYS_YIELD\n"
    },
    { "install",
      "install Makar to a hard drive",
      "Usage: install\n\nRuns the interactive Makar OS installer.\n"
    },
    { "ring3test",
      "spawn a ring-3 smoke test",
      "Usage: ring3test\n\n"
      "Spawns a minimal ring-3 task to verify user-mode entry and syscalls.\n"
    },
    { "chainload",
      "boot a sector from disk",
      "Usage: chainload <drv> [lba]\n\n"
      "Loads and executes the 512-byte sector at LBA (default 0 = MBR/GRUB).\n"
    },
    { NULL, NULL, NULL }
};

/* -------------------------------------------------------------------------
 * lsman - fixed-width two-column table
 * ---------------------------------------------------------------------- */

static void print_padded(const char *s, int width)
{
    int len = 0;
    for (const char *p = s; *p; p++) len++;
    t_writestring(s);
    for (int i = len; i < width; i++)
        t_putchar(' ');
}

void cmd_lsman(int argc, char **argv)
{
    (void)argc; (void)argv;

    t_writestring("Command         Description\n");
    t_writestring("-------         -----------\n");

    for (int k = 0; man_table[k].name; k++) {
        t_writestring("  ");
        print_padded(man_table[k].name, COL);
        t_writestring(man_table[k].brief);
        t_putchar('\n');
    }
    t_putchar('\n');
    t_writestring("Run 'man <cmd>' for full details.\n");
}

/* -------------------------------------------------------------------------
 * man
 * ---------------------------------------------------------------------- */

static void cmd_man(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: man <cmd>\n");
        return;
    }

    for (int k = 0; man_table[k].name; k++) {
        if (strcmp(man_table[k].name, argv[1]) == 0) {
            t_putchar('\n');
            print_padded(man_table[k].name, 0);
            t_writestring(" - ");
            t_writestring(man_table[k].brief);
            t_writestring("\n\n");
            t_writestring(man_table[k].body);
            t_putchar('\n');
            return;
        }
    }

    t_writestring("No manual entry for '");
    t_writestring(argv[1]);
    t_writestring("'. Try 'lsman' for a list.\n");
}

const shell_cmd_entry_t man_cmds[] = {
    { "lsman", cmd_lsman },
    { "man",   cmd_man   },
    { NULL,    NULL      },
};
