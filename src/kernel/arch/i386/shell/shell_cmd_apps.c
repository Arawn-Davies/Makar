/*
 * shell_cmd_apps.c -- application launcher shell commands.
 *
 * Commands: install  exec  eject  ring3test
 * (vix is no longer a builtin -- it ships as the userland vix.elf and is
 *  reached via PATH lookup like clock/maktop, so it runs as its own task
 *  and shows up in maktop with its own pid + memory.)
 */

#include "shell_priv.h"

#include <kernel/shell.h>
#include <kernel/vtty.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/installer.h>
#include <kernel/elf.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/ide.h>
#include <kernel/fat32.h>
#include <kernel/keyboard.h>
#include <kernel/vt.h>
#include <kernel/vesa_tty.h>
#include <string.h>

/* cmd_ring3test is defined in proc/usertest.c. */
void cmd_ring3test(int argc, char **argv);

/* ---------------------------------------------------------------------------
 * Command handlers
 * --------------------------------------------------------------------------- */

static void cmd_install(int argc, char **argv)
{
    (void)argc; (void)argv;
    installer_run();
}

#define EXEC_MAX_ARGS  128   /* room for rebuild-kernel.sh's full link line */
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

/* Last exec'd child's exit status, captured after the wait loop below.
 * sh_script.c reads this via shell_last_exec_status() to populate $? so
 * scripts can branch on the ELF's exit code (e.g. "exec foo; if [ $? -eq
 * 0 ] ...").  Reset to 0 here on every exec attempt -- if exec couldn't
 * launch (oom / pool full), $? stays at the prior value, which is fine
 * for in-kernel test scripts that key on success-marker presence rather
 * than failure-mode forensics. */
static int s_last_exec_status = 0;

int  shell_last_exec_status(void)         { return s_last_exec_status; }
void shell_reset_last_exec_status(void)   { s_last_exec_status = 0; }

/* Builtin failure-status setter: lets in-tree builtins (mkdir, etc.)
 * thread a non-zero $? back to the script interpreter without changing
 * every void-returning entry-table signature.  Calls before the next
 * `shell_reset_last_exec_status()` win the slot. */
void shell_set_last_exec_status(int s)    { s_last_exec_status = s & 0xFF; }

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
    /* Register t as VT-foreground so a user Alt+Fn excursion + return
     * routes focus + KEY_FOCUS_GAIN back to the child, not the shell. */
    if (self) vtty_set_foreground(self->tty, t);

    /* Snapshot the focused VT's backing grid BEFORE the child runs so we
     * can hand the scrollback back when it exits.  This is the same idea
     * as xterm's alternate-screen / smcup-rmcup pair (and what Linux's
     * `console_save_state` does around curses apps): the operator should
     * see their previous prompts and command history return, with the
     * fullscreen app's drawing wiped.  Heap-allocated so this scales
     * with the (cols x rows) grid; freed unconditionally after exit. */
    vt_buf_t  *vt = vtty_buf_current();
    vt_cell_t *snap_cells   = NULL;
    uint32_t   snap_n       = 0;
    uint32_t   snap_cur_col = 0, snap_cur_row = 0;
    uint32_t   snap_fg      = 0, snap_bg      = 0;
    if (vt && vt->cells) {
        snap_n     = vt->cols * vt->rows;
        snap_cells = (vt_cell_t *)kmalloc(snap_n * sizeof(vt_cell_t));
        if (snap_cells) {
            memcpy(snap_cells, vt->cells, snap_n * sizeof(vt_cell_t));
            snap_cur_col = vt->cur_col;
            snap_cur_row = vt->cur_row;
            snap_fg      = vt->fg;
            snap_bg      = vt->bg;
        }
    }

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

    /* Capture the reaped child's exit_status so sh_script.c can surface
     * it as $?.  SIGSEGV-killed tasks get exit_status set to SIGSEGV &
     * 0x7F by kill_userspace_fault (see CLAUDE.md ring-3 fault handling)
     * so non-zero status correctly flags both clean SYS_EXIT(N) calls
     * and ring-3 faults. */
    s_last_exec_status = t->exit_status & 0xFF;

    keyboard_release_task(t);
    if (self) vtty_set_foreground(self->tty, NULL);
    keyboard_set_focus(self);
    /* Defensive: a ring-3 app that enabled raw mode (kbtester etc.) is
     * expected to disable it on the way out, but if it crashed or was
     * sigint-killed it never got the chance.  Forcing cooked mode here
     * means a dead app can never lock the user out of Alt+Fn TTY switching
     * or the Ctrl+A pane prefix. */
    keyboard_set_raw(0);
    keyboard_set_scancode(0);   /* dead game (doom.elf) can't lock raw scancodes */

    /* Only clean up after FULLSCREEN apps (those that touched the
     * framebuffer via SYS_PUTCH_AT / SYS_TTY_CLEAR / SYS_DRAW_LINE).
     * Line-mode children like cat / hello / makbox-fallback-on-typo
     * write only via SYS_WRITE which scrolls normally; clearing after
     * them would wipe their output.
     *
     * Restore the pre-exec backing grid so the user's shell scrollback
     * comes back, with the app's drawing erased.  paint_buf (called
     * from shell_dispatch_argv -> shell_restore_screen) does the actual
     * pixel work; we just overwrite the cells.  We skip the old
     * shell_clear_screen() call here because that wiped the grid -- the
     * whole point of the snapshot is to keep the operator's history. */
    if (t->fb_touched && vt && vt->cells && snap_cells && snap_n == vt->cols * vt->rows) {
        memcpy(vt->cells, snap_cells, snap_n * sizeof(vt_cell_t));
        vt->cur_col = snap_cur_col;
        vt->cur_row = snap_cur_row;
        vt->fg      = snap_fg;
        vt->bg      = snap_bg;
    } else if (t->fb_touched) {
        /* No snapshot available (alloc failed or VT geometry changed
         * mid-exec, e.g. setmode while a child was running) -- fall
         * back to the old behaviour. */
        shell_clear_screen();
    }
    if (snap_cells) kfree(snap_cells);
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
        /* Unmount the sole bound HD volume (FAT32 or ext2).  Multi-mount
         * boots need explicit `umount /mnt/<name>` instead. */
        int r = vfs_umount_hd(NULL);
        if (r == -20) {
            t_writestring("eject: multiple HD volumes mounted - use 'umount /mnt/<name>'\n");
            return;
        }
        if (r != 0) {
            t_writestring("eject: no HDD volume is mounted\n");
            return;
        }
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
    { "install",   cmd_install,   1 },  /* installer TUI paints FB */
    { "exec",      cmd_exec,      1 },  /* ELF child may paint FB */
    { "eject",     cmd_eject,     0 },
    { "ring3test", cmd_ring3test, 0 },
    { NULL, NULL, 0 }
};
