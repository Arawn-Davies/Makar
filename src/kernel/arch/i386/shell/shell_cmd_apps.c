/*
 * shell_cmd_apps.c -- application launcher shell commands.
 *
 * Commands: vix  install  exec  eject  ring3test
 */

#include "shell_priv.h"

#include <kernel/shell.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/vix.h>
#include <kernel/installer.h>
#include <kernel/elf.h>
#include <kernel/task.h>
#include <kernel/heap.h>
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

/* Per-call exec params, allocated on the heap and handed off to the new
 * task via task_t.exec_params.  Replaces the old shared statics that
 * raced across shells: a second exec from another TTY would overwrite
 * the first's argv before its child had read them, producing corrupted
 * argc frames and ring-3 jumps to garbage EIPs (CS=0x3f8 panic). */
typedef struct {
    char        path[VFS_PATH_MAX];
    int         argc;
    char        argbufs[EXEC_MAX_ARGS][EXEC_ARG_MAX];
    const char *argv[EXEC_MAX_ARGS + 1];
} exec_params_t;

static void exec_task_entry(void)
{
    task_t *self = task_current();
    exec_params_t *p = (exec_params_t *)self->exec_params;
    if (!p) {
        t_writestring("exec: missing params (kernel bug)\n");
        task_exit();
    }

    /* Snapshot onto this task's stack, free the heap struct, then jump
     * into ring-3.  elf_exec never returns, so the kfree must precede
     * the call - otherwise the params leak on every successful exec. */
    char path[VFS_PATH_MAX];
    int  argc = p->argc;
    const char *argv[EXEC_MAX_ARGS + 1];
    strncpy(path, p->path, VFS_PATH_MAX - 1);
    path[VFS_PATH_MAX - 1] = '\0';
    for (int i = 0; i <= argc && i <= EXEC_MAX_ARGS; i++)
        argv[i] = p->argv[i];

    /* Re-point each argv[] at the *snapshot* argbufs - the heap copy is
     * about to be freed, so the post-kfree elf_exec must read from
     * stack-local string storage.  We could memcpy the argbufs onto the
     * stack here for full hygiene; instead leave them on the heap and
     * defer the free until elf_exec has consumed them.  Cheaper, same
     * race-freedom because nothing else writes to *this task's*
     * exec_params slot. */
    elf_exec(path, argc, argv);

    /* If elf_exec ever returns (it shouldn't), free + exit cleanly. */
    kfree(p);
    self->exec_params = NULL;
    task_exit();
}

void shell_exec_elf(const char *path, int argc, char **argv)
{
    int nargs = argc;
    if (nargs > EXEC_MAX_ARGS)
        nargs = EXEC_MAX_ARGS;

    exec_params_t *p = (exec_params_t *)kmalloc(sizeof(*p));
    if (!p) {
        t_writestring("exec: out of memory\n");
        return;
    }

    p->argc = nargs;
    for (int i = 0; i < nargs; i++) {
        strncpy(p->argbufs[i], argv[i], EXEC_ARG_MAX - 1);
        p->argbufs[i][EXEC_ARG_MAX - 1] = '\0';
        p->argv[i] = p->argbufs[i];
    }
    p->argv[nargs] = NULL;

    strncpy(p->path, path, VFS_PATH_MAX - 1);
    p->path[VFS_PATH_MAX - 1] = '\0';

    task_t *t = task_create("exec", exec_task_entry);
    if (!t) {
        kfree(p);
        t_writestring("exec: task_create failed (pool full?)\n");
        return;
    }
    t->exec_params = p;

    /* Replace the placeholder "exec" task name with the basename of
     * the program (minus a trailing ".elf") so /proc/tasks + maktop
     * + `cat /proc/tasks` show the real running binary.  Stored in
     * task_t.name_buf so the pointer outlives the path argument. */
    {
        const char *base = path;
        for (const char *q = path; *q; q++) {
            if (*q == '/') base = q + 1;
        }
        size_t n = 0;
        while (base[n] && n < sizeof(t->name_buf) - 1) {
            t->name_buf[n] = base[n];
            n++;
        }
        /* Strip a trailing ".elf" if present. */
        if (n >= 4 && t->name_buf[n-4] == '.' && t->name_buf[n-3] == 'e' &&
                       t->name_buf[n-2] == 'l' && t->name_buf[n-1] == 'f') {
            n -= 4;
        }
        t->name_buf[n] = '\0';
        t->name = t->name_buf;
    }

    task_t *self = task_current();
    keyboard_set_focus(t);

    /* Wait for the child to finish.  Ctrl+C is now delivered as SIGINT
     * straight to the focused task (the child); the kernel's default-
     * terminate action in sig_deliver kills it on the next schedule
     * pass, so the shell just yields until t->state flips to DEAD.
     * No more explicit force-kill from this side -- the previous
     * keyboard_sigint_consume polling could also race the kernel
     * (consume the flag here, child runs one more time, then dies
     * for unrelated reasons and we lose the cause attribution). */
    while (t->state != TASK_DEAD)
        task_yield();

    keyboard_release_task(t);
    keyboard_set_focus(self);
    /* Defensive: a ring-3 app that enabled raw mode (kbtester etc.) is
     * expected to disable it on the way out, but if it crashed or was
     * sigint-killed it never got the chance.  Forcing cooked mode here
     * means a dead app can never lock the user out of Alt+Fn TTY switching
     * or the Ctrl+A pane prefix. */
    keyboard_set_raw(0);

    /* Only clean up after FULLSCREEN apps (those that touched the
     * framebuffer via SYS_PUTCH_AT / SYS_TTY_CLEAR).  Line-mode
     * children like cat / hello / makbox-fallback-on-typo write only
     * via SYS_WRITE which scrolls normally; clearing after them would
     * wipe their output.  Fullscreen apps that exited cleanly already
     * called sys_shell_clear themselves -- doing it again is a no-op
     * cost but rescues SIGKILL'd ones that never got the chance. */
    if (t->fb_touched)
        shell_clear_screen();
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
