/*
 * shell_cmd_apps.c -- application launcher shell commands.
 *
 * Commands: vix  install  exec  eject  ring3test
 */

#include "shell_priv.h"

#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/vix.h>
#include <kernel/installer.h>
#include <kernel/elf.h>
#include <kernel/task.h>
#include <kernel/ide.h>
#include <kernel/fat32.h>
#include <kernel/keyboard.h>

/* cmd_ring3test is defined in proc/usertest.c. */
void cmd_ring3test(int argc, char **argv);

/* ---------------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------------- */

static void cmd_vix(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: vix <filename>\n");
        t_writestring("  Opens <filename> for editing. Creates the file on save if it does not exist.\n");
        t_writestring("  Key bindings: Arrow keys navigate | Ctrl+S save | Ctrl+Q quit\n");
        return;
    }

    const char *arg  = argv[1];
    const char *cwd  = vfs_getcwd();
    static char full_path[VFS_PATH_MAX];

    if (arg[0] == '/') {
        strncpy(full_path, arg, VFS_PATH_MAX - 1);
        full_path[VFS_PATH_MAX - 1] = '\0';
    } else {
        size_t cwd_len = strlen(cwd);
        size_t arg_len = strlen(arg);
        if (cwd_len + 1 + arg_len >= VFS_PATH_MAX) {
            t_writestring("vix: path too long\n");
            return;
        }
        size_t off = 0;
        memcpy(full_path, cwd, cwd_len); off += cwd_len;
        if (cwd[cwd_len - 1] != '/')
            full_path[off++] = '/';
        memcpy(full_path + off, arg, arg_len + 1);
    }

    vix_edit(full_path, NULL);
}

static void cmd_install(int argc, char **argv)
{
    (void)argc; (void)argv;
    installer_run();
}

#define EXEC_MAX_ARGS  16
#define EXEC_ARG_MAX   256

static char        s_exec_path[VFS_PATH_MAX];
static char        s_exec_argbufs[EXEC_MAX_ARGS][EXEC_ARG_MAX];
static const char *s_exec_argv[EXEC_MAX_ARGS + 1];
static int         s_exec_argc;

static void exec_task_entry(void)
{
    elf_exec(s_exec_path, s_exec_argc, s_exec_argv);
    task_exit();
}

void shell_exec_elf(const char *path, int argc, char **argv)
{
    int nargs = argc;
    if (nargs > EXEC_MAX_ARGS)
        nargs = EXEC_MAX_ARGS;

    s_exec_argc = nargs;
    for (int i = 0; i < nargs; i++) {
        strncpy(s_exec_argbufs[i], argv[i], EXEC_ARG_MAX - 1);
        s_exec_argbufs[i][EXEC_ARG_MAX - 1] = '\0';
        s_exec_argv[i] = s_exec_argbufs[i];
    }
    s_exec_argv[nargs] = NULL;

    strncpy(s_exec_path, path, VFS_PATH_MAX - 1);
    s_exec_path[VFS_PATH_MAX - 1] = '\0';

    task_t *t = task_create("exec", exec_task_entry);
    if (!t) {
        t_writestring("exec: task_create failed (pool full?)\n");
        return;
    }

    task_t *self = task_current();
    keyboard_set_focus(t);

    while (t->state != TASK_DEAD) {
        if (keyboard_sigint_consume()) {
            t->state = TASK_DEAD;
            t_writestring("\n^C\n");
            break;
        }
        task_yield();
    }

    keyboard_release_task(t);
    keyboard_set_focus(self);
    /* Defensive: a ring-3 app that enabled raw mode (kbtester etc.) is
     * expected to disable it on the way out, but if it crashed or was
     * sigint-killed it never got the chance.  Forcing cooked mode here
     * means a dead app can never lock the user out of Alt+Fn TTY switching
     * or the Ctrl+A pane prefix. */
    keyboard_set_raw(0);
}

static void cmd_exec(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: exec <path> [args...]\n");
        return;
    }
    /* argv[1] is the path; argv[1..] become the app's argv[0..]. */
    shell_exec_elf(argv[1], argc - 1, argv + 1);
}

static void cmd_eject(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("Usage: eject hdd|cdrom\n");
        return;
    }

    const char *target = argv[1];

    if (strcmp(target, "hdd") == 0) {
        if (!fat32_mounted()) {
            t_writestring("eject: no HDD volume is mounted\n");
            return;
        }
        fat32_unmount();
        vfs_notify_hd_unmounted();
        t_writestring("HDD volume unmounted.\n");
        return;
    }

    if (strcmp(target, "cdrom") == 0) {
        int cd_drive = -1;
        for (int i = 0; i < IDE_MAX_DRIVES; i++) {
            const ide_drive_t *d = ide_get_drive((uint8_t)i);
            if (d && d->present && d->type == IDE_TYPE_ATAPI) {
                cd_drive = i;
                break;
            }
        }

        if (cd_drive < 0) {
            t_writestring("eject: no CD-ROM drive detected\n");
            return;
        }

        vfs_notify_cdrom_ejected();

        int err = ide_eject_atapi((uint8_t)cd_drive);
        if (err) {
            t_writestring("eject: ATAPI eject command failed (err ");
            t_dec((uint32_t)err);
            t_writestring(")\n");
        } else {
            t_writestring("CD-ROM ejected.\n");
        }
        return;
    }

    t_writestring("eject: unknown target '");
    t_writestring(target);
    t_writestring("' - use 'hdd' or 'cdrom'\n");
}

/* ---------------------------------------------------------------------------
 * Module table
 * --------------------------------------------------------------------------- */

const shell_cmd_entry_t apps_cmds[] = {
    { "vix",      cmd_vix,      1 },  /* paints FB directly */
    { "install",   cmd_install,   1 },  /* installer TUI paints FB */
    { "exec",      cmd_exec,      1 },  /* ELF child may paint FB */
    { "eject",     cmd_eject,     0 },
    { "ring3test", cmd_ring3test, 0 },
    { NULL, NULL, 0 }
};
