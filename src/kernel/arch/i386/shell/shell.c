/*
 * shell.c -- kernel REPL core: line input, tokenisation, and command dispatch.
 *
 * Provides:
 *   shell_readline() - echoes input, handles backspace, ends on Enter
 *   shell_run()      - infinite prompt loop (Medli-style UX)
 *
 * Command implementations live in shell_cmds.c and shell_help.c.
 */

#include "shell_priv.h"

#include <kernel/shell.h>
#include <kernel/keyboard.h>
#include <kernel/tty.h>
#include <kernel/vesa_tty.h>
#include <kernel/vfs.h>
#include <kernel/ktest.h>

#include <string.h>

/* ---------------------------------------------------------------------------
 * Command history ring-buffer (SHELL_HISTORY_SIZE most-recent entries).
 * --------------------------------------------------------------------------- */
#define SHELL_HISTORY_SIZE 16

static char s_history[SHELL_HISTORY_SIZE][SHELL_MAX_INPUT];
static int  s_hist_count = 0;  /* valid entries (capped at SHELL_HISTORY_SIZE) */
static int  s_hist_head  = 0;  /* index of the next write slot                 */

static void history_push(const char *line)
{
    if (!line || !*line)
        return;
    strncpy(s_history[s_hist_head], line, SHELL_MAX_INPUT - 1);
    s_history[s_hist_head][SHELL_MAX_INPUT - 1] = '\0';
    s_hist_head = (s_hist_head + 1) % SHELL_HISTORY_SIZE;
    if (s_hist_count < SHELL_HISTORY_SIZE)
        s_hist_count++;
}

/* ago=0 → most-recent entry; returns NULL when out of range. */
static const char *history_get(int ago)
{
    if (ago < 0 || ago >= s_hist_count)
        return NULL;
    int idx = (s_hist_head - 1 - ago + SHELL_HISTORY_SIZE) % SHELL_HISTORY_SIZE;
    return s_history[idx];
}

/* ---------------------------------------------------------------------------
 * shell_readline – read a line from the PS/2 keyboard into buf.
 *
 * Echoes every printable character to the VGA terminal.  Handles:
 *   '\b'           – erase the previous character (if any).
 *   '\n'           – end of line.
 *   KEY_ARROW_UP   – recall older history entry.
 *   KEY_ARROW_DOWN – recall newer history entry (or restore working line).
 * Always NUL-terminates buf.  Reads at most (max - 1) characters.
 * --------------------------------------------------------------------------- */
void shell_readline(char *buf, size_t max)
{
    size_t len      = 0;
    int    hist_pos = -1;            /* -1 = not in history-navigation mode */
    char   work[SHELL_MAX_INPUT];    /* in-progress line saved on first ↑   */

    buf[0]  = '\0';
    work[0] = '\0';

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            t_putchar('\n');
            break;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                t_backspace();
            }
            hist_pos = -1;
            continue;
        }

        /* ---- Arrow-key history navigation ---- */
        if (c == KEY_ARROW_UP || c == KEY_ARROW_DOWN) {
            int new_pos;

            if (c == KEY_ARROW_UP) {
                if (hist_pos < 0) {
                    /* First ↑ press: save the in-progress line. */
                    strncpy(work, buf, max - 1);
                    work[max - 1] = '\0';
                }
                new_pos = (hist_pos < 0) ? 0 : hist_pos + 1;
                if (new_pos >= s_hist_count)
                    new_pos = s_hist_count - 1;
            } else {
                new_pos = hist_pos - 1;
            }

            const char *entry;
            if (new_pos < 0) {
                /* Went past the newest entry → restore the working line. */
                entry    = work;
                hist_pos = -1;
            } else {
                entry = history_get(new_pos);
                if (!entry)
                    continue;
                hist_pos = new_pos;
            }

            /* Erase the current line on-screen. */
            for (size_t i = 0; i < len; i++)
                t_backspace();

            /* Load and display the recalled line. */
            strncpy(buf, entry, max - 1);
            buf[max - 1] = '\0';
            len = strlen(buf);
            t_writestring(buf);
            continue;
        }

        /* Accept printable ASCII; silently drop everything else. */
        if (c < 0x20 || c > 0x7E)
            continue;

        /* Any typed character exits history-navigation mode. */
        hist_pos = -1;

        if (len < max - 1) {
            buf[len++] = c;
            buf[len]   = '\0';
            t_putchar(c);
        }
    }

    buf[len] = '\0';
}

/* ---------------------------------------------------------------------------
 * shell_parse – split line in-place into at most max_args tokens.
 *
 * Tokens are separated by spaces.  Returns the number of tokens found.
 * --------------------------------------------------------------------------- */
static int shell_parse(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* Skip leading spaces. */
        while (*p == ' ')
            p++;

        if (!*p)
            break;

        argv[argc++] = p;

        /* Advance to the end of the token. */
        while (*p && *p != ' ')
            p++;

        if (*p)
            *p++ = '\0';
    }

    return argc;
}

/* ---------------------------------------------------------------------------
 * Command dispatch – enum + lookup table + switch/case/default.
 *
 * To add a new command: add a CMD_FOO entry to the enum, add a row to
 * cmd_table[], add a case in shell_run().
 * --------------------------------------------------------------------------- */

typedef enum {
    CMD_HELP,
    CMD_CLEAR,
    CMD_ECHO,
    CMD_MEMINFO,
    CMD_UPTIME,
    CMD_VERSION,
    CMD_TASKS,
    CMD_LSDISKS,
    CMD_LSPART,
    CMD_MKPART,
    CMD_READSECTOR,
    CMD_SETMODE,
    CMD_SHUTDOWN,
    CMD_REBOOT,
    CMD_KTEST,
    /* FAT32 commands */
    CMD_MOUNT,
    CMD_UMOUNT,
    CMD_LS,
    CMD_CAT,
    CMD_CD,
    CMD_MKDIR,
    CMD_MKFS,
    /* ISO9660 commands */
    CMD_ISOLS,
    /* Installer */
    CMD_INSTALL,
    CMD_VICS,
    CMD_EJECT,
    CMD_RING3TEST,
    CMD_UNKNOWN,
} shell_cmd_t;

typedef struct {
    const char *name;
    shell_cmd_t id;
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    { "help",       CMD_HELP       },
    { "clear",      CMD_CLEAR      },
    { "echo",       CMD_ECHO       },
    { "meminfo",    CMD_MEMINFO    },
    { "uptime",     CMD_UPTIME     },
    { "version",    CMD_VERSION    },
    { "tasks",      CMD_TASKS      },
    { "lsdisks",    CMD_LSDISKS    },
    { "lspart",     CMD_LSPART     },
    { "mkpart",     CMD_MKPART     },
    { "readsector", CMD_READSECTOR },
    { "setmode",    CMD_SETMODE    },
    { "shutdown",   CMD_SHUTDOWN   },
    { "reboot",     CMD_REBOOT     },
    { "ktest",      CMD_KTEST      },
    /* FAT32 commands */
    { "mount",      CMD_MOUNT      },
    { "umount",     CMD_UMOUNT     },
    { "ls",         CMD_LS         },
    { "cat",        CMD_CAT        },
    { "cd",         CMD_CD         },
    { "mkdir",      CMD_MKDIR      },
    { "mkfs",       CMD_MKFS       },
    /* ISO9660 commands */
    { "isols",      CMD_ISOLS      },
    /* Installer */
    { "install",    CMD_INSTALL    },
    { "vics",       CMD_VICS       },
    { "eject",      CMD_EJECT      },
    { "ring3test",  CMD_RING3TEST  },
};

#define CMD_TABLE_SIZE ((int)(sizeof(cmd_table) / sizeof(cmd_table[0])))

static shell_cmd_t shell_lookup(const char *name)
{
    for (int i = 0; i < CMD_TABLE_SIZE; i++) {
        if (strcmp(cmd_table[i].name, name) == 0)
            return cmd_table[i].id;
    }
    return CMD_UNKNOWN;
}

/* ---------------------------------------------------------------------------
 * shell_print_prompt – print the interactive prompt showing the VFS CWD.
 *
 * Format:  root@makar /hd/boot~>
 * --------------------------------------------------------------------------- */
static void shell_print_prompt(void)
{
    t_writestring(SHELL_USERNAME "@" SHELL_HOSTNAME " ");
    t_writestring(vfs_getcwd());
    t_writestring("~> ");
}

/* ---------------------------------------------------------------------------
 * shell_run – infinite REPL loop.  Never returns.
 * --------------------------------------------------------------------------- */
void shell_run(void)
{
    static char buf[SHELL_MAX_INPUT];
    char *argv[SHELL_MAX_ARGS];

    /*
     * Apply the Medli-compatible colour scheme: white text on blue background.
     * This matches Medli's BeforeRun() which sets ConsoleColor.Blue background
     * and ConsoleColor.White foreground before clearing the screen.
     */
    terminal_set_colorscheme(SHELL_COLOR_VGA);
    if (vesa_tty_is_ready()) {
        vesa_tty_setcolor(SHELL_FG_RGB, SHELL_BG_RGB);
        vesa_tty_clear();
    }

    t_writestring("Makar -- version " SHELL_VERSION
                  ", build: " BUILD_DATE " " BUILD_TIME "\n");
    t_writestring("The GCC/C++ sibling of Medli\n");
    t_writestring(COPYRIGHT "\n");
    t_writestring("Released under the BSD-3 Clause Clear license\n");
    t_writestring("\n");
    t_writestring("Type 'help' for a list of commands.\n");
    t_writestring("Welcome back, " SHELL_USERNAME "!\n\n");

    /* Initialise the VFS: probe for CD-ROM, set CWD to "/". */
    vfs_init();
    /* Auto-mount the appropriate filesystem based on the boot device. */
    vfs_auto_mount();

    while (1) {
        shell_print_prompt();
        shell_readline(buf, SHELL_MAX_INPUT);

        /* Save the raw input line before shell_parse() tokenises it in-place. */
        history_push(buf);

        int argc = shell_parse(buf, argv, SHELL_MAX_ARGS);
        if (argc == 0)
            continue;

        switch (shell_lookup(argv[0])) {
        case CMD_HELP:       cmd_help();                  break;
        case CMD_CLEAR:      cmd_clear();                 break;
        case CMD_ECHO:       cmd_echo(argc, argv);        break;
        case CMD_MEMINFO:    cmd_meminfo();               break;
        case CMD_UPTIME:     cmd_uptime();                break;
        case CMD_VERSION:    cmd_version();               break;
        case CMD_TASKS:      cmd_tasks();                 break;
        case CMD_LSDISKS:    cmd_lsdisks();               break;
        case CMD_LSPART:     cmd_lspart(argc, argv);      break;
        case CMD_MKPART:     cmd_mkpart(argc, argv);      break;
        case CMD_READSECTOR: cmd_readsector(argc, argv);  break;
        case CMD_SETMODE:    cmd_setmode(argc, argv);     break;
        case CMD_SHUTDOWN:   cmd_shutdown();              break;
        case CMD_REBOOT:     cmd_reboot();               break;
        case CMD_KTEST:      ktest_run_all();            break;
        /* FAT32 commands */
        case CMD_MOUNT:      cmd_mount(argc, argv);       break;
        case CMD_UMOUNT:     cmd_umount();                break;
        case CMD_LS:         cmd_ls(argc, argv);          break;
        case CMD_CAT:        cmd_cat(argc, argv);         break;
        case CMD_CD:         cmd_cd(argc, argv);          break;
        case CMD_MKDIR:      cmd_mkdir(argc, argv);       break;
        case CMD_MKFS:       cmd_mkfs(argc, argv);        break;
        /* ISO9660 commands */
        case CMD_ISOLS:      cmd_isols(argc, argv);       break;
        /* Installer */
        case CMD_INSTALL:    cmd_install();               break;
        case CMD_VICS:       cmd_vics(argc, argv);        break;
        case CMD_EJECT:      cmd_eject(argc, argv);       break;
        case CMD_RING3TEST:  cmd_ring3test();             break;
        default:
            /* Medli-style error: red text, matching CommandConsole.cs */
            t_setcolor(SHELL_ERROR_COLOR_VGA);
            t_writestring("The command '");
            t_writestring(argv[0]);
            t_writestring("' is not supported. Please type help for more information.\n\n");
            t_setcolor(SHELL_COLOR_VGA);
            break;
        }
    }
}
