/*
 * syscall.c - int 0x80 syscall dispatcher (Linux i386 ABI subset).
 *
 * Supported calls:
 *   SYS_EXIT  (1)  - terminate current task
 *   SYS_READ  (3)  - read from fd
 *   SYS_WRITE (4)  - write to fd
 *   SYS_OPEN  (5)  - open a VFS path, returns fd
 *   SYS_CLOSE (6)  - close a fd
 *   SYS_LSEEK (19) - seek within an open file fd
 *   SYS_BRK   (45) - expand/query user-space heap break
 *   SYS_YIELD (158)- cooperative yield
 *   SYS_DEBUG (100)- debug checkpoint (Makar extension)
 *
 * File descriptors:
 *   Each task owns its own fd table (kernel/fd.h). On task_create, fds
 *   0/1/2 are pre-bound to stdin (keyboard), stdout (VGA), stderr
 *   (VGA + serial). SYS_OPEN allocates the lowest free slot.
 *
 * Interrupts stay disabled for the duration of the syscall (the isr_common_stub
 * begins with cli). keyboard_getchar() task_yield()s internally so other tasks
 * - which have IF=1 in their saved EFLAGS - can receive IRQ1 and fill the
 * keyboard ring buffer while this task waits.
 */

#include <kernel/syscall.h>
#include <kernel/isr.h>
#include <kernel/task.h>
#include <kernel/descr_tbl.h>
#include <kernel/fd.h>
#include <kernel/signal.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/installer.h>   /* install_exec_* (SYS_INSTALL_EXEC) */
#include <kernel/mouse.h>
#include <kernel/shell.h>
#include <kernel/vfs.h>
#include <kernel/pagecache.h>
#include <kernel/video.h>
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/surface.h>
#include <kernel/elf.h>
#include <kernel/serial.h>
#include <kernel/vga.h>
#include <kernel/vesa_tty.h>
#include <kernel/vesa.h>
#include <kernel/vtty.h>
#include <kernel/vt.h>
#include <kernel/ide.h>
#include <kernel/pci.h>
#include <kernel/netdev.h>
#include <kernel/net_lwip.h>
#include <kernel/wget.h>
#include <kernel/timer.h>
#include <kernel/rtc.h>
#include <string.h>
#include <kernel/ktest.h>
#include <kernel/admin.h>
#include <kernel/auth.h>

/* USER_STACK_TOP / USER_STACK_PAGES - matches elf.c; defined locally to
 * avoid pulling that header into syscall.c just for these constants.
 * Stack grows down from USER_STACK_TOP in USER_STACK_PAGES mapped 4 KiB
 * pages; keep these two in sync with the matching defines in elf.c. */
#define USER_STACK_TOP    0xBFFF0000u
#define USER_STACK_PAGES  8u

/* Standard CGA/VGA 16-colour palette for SYS_PUTCH_AT VESA rendering. */
static const uint32_t s_vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static int stdin_pipe_getchar(task_t *t)
{
    fd_entry_t *e = fd_get(t ? t->fd_table : NULL, 0);
    if (!e || e->kind != FD_KIND_PIPE)
        return -2;
    if (e->pipe_is_writer || !e->pipe)
        return -1;

    pipe_ring_t *r = e->pipe;
    for (;;) {
        if (r->head != r->tail) {
            unsigned char c = r->buf[r->tail % PIPE_RING_CAP];
            r->tail++;
            return (int)c;
        }
        if (r->refcount_w == 0)
            return -1;
        if (e->flags & FD_FLAG_NONBLOCK)
            return -11;
        task_yield();
    }
}

/* ---- cell-API -> ANSI bridge (GUI terminal) -----------------------------
 * A task forked by the GUI terminal (mxterm) has no live VT slot and its
 * stdout is a pipe, so its cell-API calls (SYS_PUTCH_AT / SET_CURSOR /
 * TTY_CLEAR) have nowhere to land.  Translate them to ANSI escapes written to
 * fd 1 -- mxterm's vt100 emulator renders them -- so every cell-API TUI app
 * (maktop, vix, cfdisk, the installer, ...) works inside a terminal window
 * unchanged.  The real text-VT path (vtty_buf_current() != NULL) is untouched;
 * grandchildren inherit the piped fd 1 so they're covered too.
 */
static fd_entry_t *ansi_bridge_fd(void)
{
    task_t *cur = task_current();
    if (!cur || vtty_buf_current() != NULL) return NULL;   /* has a live VT */
    fd_entry_t *e = fd_get(cur->fd_table, 1);
    if (e && e->kind == FD_KIND_PIPE && e->pipe_is_writer && e->pipe) return e;
    return NULL;
}
static void ansi_pipe_write(fd_entry_t *e, const char *s, uint32_t n)
{
    pipe_ring_t *r = e->pipe;
    uint32_t w = 0; uint32_t spins = 0;
    while (w < n) {
        if (r->refcount_r == 0) break;                     /* reader gone */
        uint32_t space = PIPE_RING_CAP - (r->head - r->tail);
        if (space == 0) { if (++spins > 200000u) break; task_yield(); continue; }
        uint32_t chunk = n - w; if (chunk > space) chunk = space;
        for (uint32_t i = 0; i < chunk; i++)
            r->buf[(r->head + i) % PIPE_RING_CAP] = (uint8_t)s[w + i];
        r->head += chunk; w += chunk; spins = 0;
    }
}
static int ansi_u(char *b, unsigned v)
{ char t[10]; int n = 0; if (!v) { b[0] = '0'; return 1; }
  while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
  for (int i = 0; i < n; i++) b[i] = t[n-1-i]; return n; }
/* VGA colour index -> ANSI index (VGA swaps red/blue vs ANSI). */
static const unsigned char s_vga2ansi[8] = {0,4,2,6,1,5,3,7};
static int ansi_sgr(char *b, unsigned char vga)
{   unsigned f = vga & 0x0F, g = (vga >> 4) & 0x0F;
    unsigned fc = (f < 8) ? 30u + s_vga2ansi[f]   : 90u  + s_vga2ansi[f-8];
    unsigned bc = (g < 8) ? 40u + s_vga2ansi[g]   : 100u + s_vga2ansi[g-8];
    int o = 0; b[o++] = 0x1b; b[o++] = '['; b[o++] = '0'; b[o++] = ';';
    o += ansi_u(b+o, fc); b[o++] = ';'; o += ansi_u(b+o, bc); b[o++] = 'm'; return o; }
static int ansi_cup(char *b, unsigned row, unsigned col)
{   int o = 0; b[o++] = 0x1b; b[o++] = '['; o += ansi_u(b+o, row+1); b[o++] = ';';
    o += ansi_u(b+o, col+1); b[o++] = 'H'; return o; }

/* -------------------------------------------------------------------------
 * Generic in-kernel terminal I/O on the calling task's standard streams
 * (kernel/fd.h) -- the kernel-side analogue of write(1)/read(0).  In-kernel
 * TUI code (e.g. the installer) uses these to speak to whatever terminal the
 * invoking ring-3 task owns: a real text VT (bytes reach the framebuffer
 * console's vt_putchar ANSI parser) or an mxterm window (bytes reach the
 * client over a pipe).  This file provides the implementation; it deliberately
 * knows nothing about who calls it.
 * ------------------------------------------------------------------------- */
long kfd_stdout_write(const char *buf, unsigned int len)
{
    task_t     *cur = task_current();
    fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, 1);
    if (!e) return -1;
    if (e->kind == FD_KIND_PIPE && e->pipe_is_writer && e->pipe) {
        ansi_pipe_write(e, buf, len);
        return (long)len;
    }
    if (e->kind == FD_KIND_VGA || e->kind == FD_KIND_VGA_SERIAL) {
        for (unsigned int i = 0; i < len; i++) t_putchar(buf[i]);
        if (e->kind == FD_KIND_VGA_SERIAL && !g_serial_verbose)
            for (unsigned int i = 0; i < len; i++) Serial_WriteChar(buf[i]);
        return (long)len;
    }
    if (e->kind == FD_KIND_SERIAL) {
        for (unsigned int i = 0; i < len; i++) Serial_WriteChar(buf[i]);
        return (long)len;
    }
    return -1;
}

int kfd_stdout_is_pipe(void)
{
    task_t     *cur = task_current();
    fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, 1);
    return (e && e->kind == FD_KIND_PIPE && e->pipe_is_writer && e->pipe) ? 1 : 0;
}

int kfd_stdin_getbyte(void)
{
    task_t     *cur = task_current();
    fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, 0);
    if (e && e->kind == FD_KIND_PIPE && !e->pipe_is_writer && e->pipe) {
        pipe_ring_t *r = e->pipe;
        for (;;) {
            if (r->head != r->tail) {
                unsigned char b = r->buf[r->tail % PIPE_RING_CAP];
                r->tail++;
                return (int)b;
            }
            if (r->refcount_w == 0) return -1;   /* writer (mxterm) gone */
            task_yield();
        }
    }
    /* Real text VT (or no fd 0): raw single key, so arrows/Esc/Enter arrive
     * un-line-buffered -- exactly what the wizard's getkey() expects. */
    return (int)(unsigned char)keyboard_getchar();
}

void kfd_term_size(unsigned int *cols, unsigned int *rows)
{
    unsigned int c = 80, rw = 25;
    task_t     *cur = task_current();
    fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, 1);
    if (e && e->kind == FD_KIND_PIPE && e->pipe && e->pipe->cols && e->pipe->rows) {
        c = e->pipe->cols; rw = e->pipe->rows;
    } else if (vesa_tty_is_ready()) {
        c = vesa_tty_get_cols(); rw = vesa_tty_usable_rows();
    }
    if (cols) *cols = c;
    if (rows) *rows = rw;
}

/* Callback + context for SYS_LS_DIR using vfs_complete. */
typedef struct { char *buf; uint32_t cap; uint32_t off; } ls_ctx_t;
static void ls_cb(const char *name, int is_dir, void *ctx)
{
    ls_ctx_t *c = (ls_ctx_t *)ctx;
    for (const char *p = name; *p && c->off < c->cap - 2; p++)
        c->buf[c->off++] = *p;
    if (is_dir && c->off < c->cap - 2)
        c->buf[c->off++] = '/';
    if (c->off < c->cap - 1)
        c->buf[c->off++] = '\n';
    c->buf[c->off] = '\0';
}

/* Callback + context for SYS_READDIR.  The found name/type live in the ctx (on
 * the caller's stack), not in file-scope statics -- so two tasks running
 * readdir concurrently under preemptible syscalls don't clobber each other. */
struct rd_ctx {
    uint32_t target; uint32_t cur; int found;
    char     name[DIRENT_NAME_MAX];
    int      is_dir;
};
static void readdir_collect_cb(const char *n, int is_dir, void *vctx)
{
    struct rd_ctx *c = (struct rd_ctx *)vctx;
    if (c->found) return;
    if (c->cur == c->target) {
        uint32_t i = 0;
        while (n[i] && i < DIRENT_NAME_MAX - 1) { c->name[i] = n[i]; i++; }
        c->name[i] = '\0';
        c->is_dir = is_dir;
        c->found = 1;
    }
    c->cur++;
}

/* -------------------------------------------------------------------------
 * Checkpoint tracking
 * ------------------------------------------------------------------------- */

volatile uint32_t g_ring3_last_cp = 0;

/* -------------------------------------------------------------------------
 * syscall_dispatch
 * ------------------------------------------------------------------------- */

static void syscall_dispatch_inner(registers_t *regs)
{
    switch (regs->eax) {

    /* ------------------------------------------------------------------
     * SYS_EXIT(1): terminate the calling task.
     * EBX = exit status (ignored for now).
     * ------------------------------------------------------------------ */
    case SYS_EXIT: {
        task_t *t = task_current();
        if (t) t->exit_status = (int)regs->ebx;
        Serial_WriteString("[sys_exit] task pid=");
        Serial_WriteDec(t ? (uint32_t)t->pid : 0u);
        Serial_WriteString(" status=");
        Serial_WriteDec((uint32_t)regs->ebx);
        Serial_WriteString(" -> task_exit()\n");
        task_exit();   /* does not return */
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_WAIT4(114): reap a child task.
     *   EBX = pid (-1 = any child, > 0 = specific child)
     *   ECX = int *status (writable; may be NULL)
     *   EDX = options (WNOHANG = 1)
     *   ESI = rusage* (ignored)
     *
     * Behaviour:
     *   - scan task pool for the caller's children (parent_pid == me->pid)
     *   - if a matching ZOMBIE is found: copy out exit_status, transition
     *     to DEAD (slot becomes reclaimable), return its pid
     *   - if no zombies but caller has live children and !WNOHANG: yield
     *     and retry
     *   - if WNOHANG and no zombies: return 0
     *   - if no children at all: return -ECHILD
     * ------------------------------------------------------------------ */
    case SYS_WAIT4: {
        int   want_pid = (int)regs->ebx;
        int  *ustatus  = (int *)(uintptr_t)regs->ecx;
        int   options  = (int)regs->edx;

        task_t *me = task_current();
        if (!me) { regs->eax = (uint32_t)-1; break; }

        for (;;) {
            int has_children = 0;
            int reaped       = 0;
            for (int i = 0; i < task_count(); i++) {
                task_t *c = task_get(i);
                if (!c) continue;
                if (c->parent_pid != me->pid) continue;
                if (c->state == TASK_DEAD)    continue;
                has_children = 1;
                if (c->state == TASK_ZOMBIE &&
                    (want_pid < 0 || c->pid == want_pid)) {
                    if (ustatus)
                        *ustatus = c->exit_status;
                    int cpid = c->pid;
                    int child_fb = c->fb_touched;
                    c->state = TASK_DEAD;
                    /* Counterpart to SYS_EXECVE's "child takes focus" rule:
                     * when the reaper sees the foreground child go zombie,
                     * hand focus back to the wait4-ing parent so its REPL
                     * (sh.elf, kernel shell, ...) becomes the next reader. */
                    keyboard_set_focus(me);
                    vtty_set_foreground(me->tty, me);
                    /* Userspace sh.elf runs fullscreen apps via fork+execve+
                     * wait4; unlike the kernel shell it has no snapshot/
                     * restore path.  Clear the screen on the child's way out
                     * so the next prompt lands on a blank slate, not the
                     * app's frozen last frame. */
                    if (child_fb) {
                        if (me->tty >= 0 && me->tty < VTTY_MAX) {
                            vt_buf_t *pvt = vtty_buf(me->tty);
                            if (vesa_tty_is_ready()) {
                                vesa_pane_t *_dp = vesa_tty_default_pane();
                                if (_dp && (me->disp_fg_saved | me->disp_bg_saved)) {
                                    _dp->fg = me->disp_fg_saved;
                                    _dp->bg = me->disp_bg_saved;
                                }
                                if (me->tty != VTTY_ROOT_SLOT && pvt) {
                                    /* Normal VT: child wrote into this buffer,
                                     * clear it so the parent's next prompt
                                     * lands on a blank slate.  Restore the VT's
                                     * scheme straight from the fork snapshot --
                                     * not `saved ? saved : pvt`, because a real
                                     * scheme colour can be 0x000000 (black fg on
                                     * the black-on-white VT) and the falsy-zero
                                     * fallback would keep vix's clobbered colour. */
                                    vt_set_color(pvt, me->disp_fg_saved, me->disp_bg_saved);
                                    vt_clear(pvt);
                                } else if (pvt) {
                                    /* Root slot (mak.sh0): the fullscreen child
                                     * ran as TASK_TTY_NONE and drew straight to
                                     * the framebuffer without ever touching
                                     * mak.sh0's backing buffer.  Wipe those raw
                                     * pixels and repaint the console buffer so
                                     * the hand-back is identical to the other
                                     * VTs -- vix/maktop/etc. don't leave a frozen
                                     * frame behind.  makmux also lands here, and
                                     * its buffer is intact (it only drew status
                                     * rows + child VTs), so this still restores
                                     * the pre-makmux screen, just on a clean FB. */
                                    vesa_tty_setcolor(me->disp_fg_saved, me->disp_bg_saved);
                                    vesa_tty_clear();
                                    vesa_tty_paint_buf(pvt);
                                }
                            }
                            vtty_request_repaint(me->tty);
                        } else if (vesa_tty_is_ready()) {
                            /* Truly no-VT parent (shouldn't occur in normal
                             * operation now that mak.sh0 uses VTTY_ROOT_SLOT,
                             * but keep as a safe fallback). */
                            vesa_pane_t *_dp = vesa_tty_default_pane();
                            if (_dp && (me->disp_fg_saved | me->disp_bg_saved)) {
                                vesa_tty_setcolor(me->disp_fg_saved, me->disp_bg_saved);
                            }
                            vesa_tty_clear();
                        }
                    }
                    regs->eax = (uint32_t)cpid;
                    Serial_WriteString("[sys_wait4] parent pid=");
                    Serial_WriteDec((uint32_t)me->pid);
                    Serial_WriteString(" reaped child pid=");
                    Serial_WriteDec((uint32_t)cpid);
                    Serial_WriteString(" status=");
                    Serial_WriteDec((uint32_t)c->exit_status);
                    Serial_WriteString("\n");
                    reaped = 1;
                    break;
                }
            }
            if (reaped) break;
            if (!has_children) { regs->eax = (uint32_t)-10; break; }   /* -ECHILD */
            if (options & 1)   { regs->eax = 0; break; }               /* WNOHANG */
            task_yield();
        }
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_EXECVE(11): replace current task's address space with a new
     * ELF.  EBX=path, ECX=argv (NULL-terminated), EDX=envp (ignored).
     *
     * On success, elf_exec swaps CR3, frees the old PD, and iret's to
     * the new entry point -- this case never falls through.  Argv
     * strings live in the caller's about-to-be-freed user PD, so we
     * copy them into kernel scratch before invoking elf_exec.
     *
     * Static scratch buffers are safe because syscalls are serialised
     * (cli at entry); we never have two execves in flight at once.
     * ------------------------------------------------------------------ */
    case SYS_EXECVE: {
        const char  *upath = (const char *)(uintptr_t)regs->ebx;
        char *const *uargv = (char *const *)(uintptr_t)regs->ecx;
        /* envp deliberately ignored: Makar has no environment yet. */

        if (!upath) { regs->eax = (uint32_t)-14; break; }   /* -EFAULT */

        /* Serialise execve so the shared argv scratch below is safe under
         * preemptible syscalls.  elf_exec releases the lock once argv is packed
         * onto the new stack (covering its no-return success path); we release
         * it here on the error-return path. */
        execve_lock();

        enum { EXECVE_MAX_ARGC = 128, EXECVE_ARG_MAX = 256 };  /* full kernel-rebuild link line */
        static char  s_path[256];
        static char  s_argbuf[EXECVE_MAX_ARGC * EXECVE_ARG_MAX];
        static char *s_argv[EXECVE_MAX_ARGC + 1];

        /* Copy path. */
        size_t pi = 0;
        while (upath[pi] && pi < sizeof(s_path) - 1) { s_path[pi] = upath[pi]; pi++; }
        s_path[pi] = '\0';

        /* Copy argv strings.  argv[0] convention is the program name;
         * shell-side exec already supplies it that way.  Stop on first
         * NULL pointer (POSIX argv terminator). */
        int kargc = 0;
        if (uargv) {
            for (; kargc < EXECVE_MAX_ARGC; kargc++) {
                const char *us = uargv[kargc];
                if (!us) break;
                char *dst = s_argbuf + kargc * EXECVE_ARG_MAX;
                size_t i = 0;
                while (us[i] && i < EXECVE_ARG_MAX - 1) { dst[i] = us[i]; i++; }
                dst[i] = '\0';
                s_argv[kargc] = dst;
            }
        }
        s_argv[kargc] = NULL;

        /* A forked userspace shell child inherits the parent's task name
         * until execve replaces the image.  Rename ordinary exec targets
         * to their basename (without .elf) so /proc/tasks and maktop show
         * makmux, tcc, etc.  Preserve mak.shN when makmux's VT children
         * exec /apps/sh.elf; those task names are the terminal identity. */
        {
            task_t *me = task_current();
            const char *base = s_path;
            for (const char *q = s_path; *q; q++)
                if (*q == '/') base = q + 1;
            int is_sh = strcmp(base, "sh.elf") == 0 || strcmp(base, "sh") == 0;
            int is_maksh = me && me->name &&
                           me->name[0] == 'm' && me->name[1] == 'a' &&
                           me->name[2] == 'k' && me->name[3] == '.' &&
                           me->name[4] == 's' && me->name[5] == 'h';
            if (me && !(is_sh && is_maksh)) {
                size_t n = 0;
                while (base[n] && n < sizeof(me->name_buf) - 1) {
                    me->name_buf[n] = base[n];
                    n++;
                }
                if (n >= 4 && me->name_buf[n-4] == '.' &&
                              me->name_buf[n-3] == 'e' &&
                              me->name_buf[n-2] == 'l' &&
                              me->name_buf[n-1] == 'f') {
                    n -= 4;
                }
                me->name_buf[n] = '\0';
                me->name = me->name_buf;
            }
            if (me && me->tty == VTTY_ROOT_SLOT && is_sh &&
                kargc >= 2 && strcmp(s_argv[1], "--login") == 0) {
                vtty_register_root_text_task(me);
            }
            if (me && me->tty == VTTY_ROOT_SLOT &&
                (strcmp(base, "gui.elf") == 0 || strcmp(base, "gui") == 0)) {
                vtty_register_root_gui(me);
                g_gui_session = 1;          /* this session is now a GUI one */
            }
        }

        /* POSIX: execve resets all caught signal handlers to SIG_DFL.
         * SIG_IGN is also reset (Makar's sig_task_init clears everything,
         * matching the simple-is-better choice). */
        sig_task_init(task_current());

        /* Job-control shorthand: the task running execve is conventionally
         * the next foreground process for its VT.  Pre-userspace-shell,
         * shell_cmd_apps did this via keyboard_set_focus before exec; now
         * that ring-3 shells (sh.elf) drive their own fork+execve, the
         * kernel does the transfer so userspace doesn't need an extra
         * syscall + race window between fork and focus-transfer.  No full
         * process-group / tcsetpgrp model yet -- this is the simple "the
         * thing you just exec'd takes the keyboard" rule. */
        task_t *me = task_current();
        if (me) {
            fd_entry_t *stdin_e = fd_get(me->fd_table, 0);
            if (stdin_e && stdin_e->kind == FD_KIND_KEYBOARD) {
                keyboard_set_focus(me);
                vtty_set_foreground(me->tty, me);
            }
        }

        /* elf_exec swaps the PD and iret's to the new entry on success
         * (never returns).  Any return value here means it failed; pass
         * the negative errno back to the caller via EAX. */
        int rc = elf_exec(s_path, kargc, (const char *const *)s_argv);
        execve_unlock();   /* only reached when elf_exec failed (success no-returns) */
        regs->eax = (uint32_t)(int32_t)rc;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_FORK(2): COW-clone the calling task.
     * Returns child pid in parent, 0 in child, -EAGAIN on failure.
     * Child returns through fork_child_iret, never through this dispatch.
     * ------------------------------------------------------------------ */
    case SYS_FORK: {
        /* Snapshot the parent's current display colours before forking.
         * SYS_WAIT4 reads these back when a fb_touched child is reaped so
         * the parent's palette (e.g. white-on-blue for mak.sh0) is
         * restored before the screen is cleared rather than inheriting
         * whatever the last VT child was drawing with. */
        task_t *me_fork = task_current();
        if (me_fork && vesa_tty_is_ready()) {
            vesa_pane_t *_dp = vesa_tty_default_pane();
            if (_dp) {
                me_fork->disp_fg_saved = _dp->fg;
                me_fork->disp_bg_saved = _dp->bg;
            }
        }
        task_t *child = task_fork(regs);
        if (!child) {
            regs->eax = (uint32_t)-11;   /* -EAGAIN */
        } else {
            Serial_WriteString("[sys_fork] parent pid=");
            Serial_WriteDec((uint32_t)task_current()->pid);
            Serial_WriteString(" -> child pid=");
            Serial_WriteDec((uint32_t)child->pid);
            Serial_WriteString("\n");
            regs->eax = (uint32_t)child->pid;
        }
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_READ(3): read bytes from a file descriptor.
     * EBX = fd, ECX = buf, EDX = len
     * Returns: bytes read (EAX), 0 on EOF, (uint32_t)-1 on error.
     *
     * The fd is looked up in the calling task's per-task fd table.
     * KEYBOARD: line-buffered stdin via shell_readline.
     * FILE:     reads from the in-memory buffer opened by SYS_OPEN.
     * Other kinds (VGA / serial-out) are not readable.
     * ------------------------------------------------------------------ */
    case SYS_READ: {
        int      fd  = (int)regs->ebx;
        char    *buf = (char *)(uintptr_t)regs->ecx;
        uint32_t len = regs->edx;

        if (!buf || len == 0) { regs->eax = 0; break; }

        task_t     *cur = task_current();
        fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, fd);
        if (!e) { regs->eax = (uint32_t)-1; break; }

        if (e->kind == FD_KIND_KEYBOARD) {
            /* Non-blocking path (O_NONBLOCK on stdin): poll once and
             * either deliver a single raw byte or return -EAGAIN.  No
             * line buffering / echo / editing; callers asking for raw
             * mode opt in deliberately.  Pairs with sys_fcntl(F_SETFL). */
            if (e->flags & FD_FLAG_NONBLOCK) {
                unsigned char c = keyboard_poll();
                if (c == 0) {
                    regs->eax = (uint32_t)-11;  /* -EAGAIN */
                } else {
                    buf[0] = (char)c;
                    regs->eax = 1u;
                }
                break;
            }
            /* Line-buffered stdin with echo, backspace, and cursor editing.
             * On the stack (not static) so concurrent readers under preemptible
             * syscalls don't share one line buffer. */
            char s_stdin_line[256];
            uint32_t cap = (len < sizeof(s_stdin_line)) ? len : (uint32_t)sizeof(s_stdin_line);
            shell_readline(s_stdin_line, (size_t)cap);
            uint32_t n = (uint32_t)strlen(s_stdin_line);
            /* Append '\n' so callers see a complete line (like a real terminal). */
            if (n < cap - 1) { s_stdin_line[n++] = '\n'; s_stdin_line[n] = '\0'; }
            if (n > len) n = len;
            memcpy(buf, s_stdin_line, n);
            regs->eax = n;
        } else if (e->kind == FD_KIND_FILE && e->lazy) {
            /* Demand-streamed read-only file: serve via the page cache. */
            long r = pagecache_read(e->path, e->size, e->pos, buf, len);
            if (r < 0) { regs->eax = (uint32_t)-1; }
            else { e->pos += (uint32_t)r; regs->eax = (uint32_t)r; }
        } else if (e->kind == FD_KIND_FILE) {
            uint32_t avail = e->size - e->pos;
            uint32_t n     = (len < avail) ? len : avail;
            if (n > 0) {
                memcpy(buf, e->data + e->pos, n);
                e->pos += n;
            }
            regs->eax = n;   /* 0 signals EOF when avail was 0 */
        } else if (e->kind == FD_KIND_BLOCKDEV) {
            long r = vfs_blockdev_pread(e->dev_node, buf, len, e->pos);
            if (r < 0) { regs->eax = (uint32_t)-1; }
            else { e->pos += (uint32_t)r; regs->eax = (uint32_t)r; }
        } else if (e->kind == FD_KIND_PIPE) {
            /* Reader on a pipe: spin-yield until data appears, the writers
             * all close (EOF -> return 0), or the fd is non-blocking. */
            if (e->pipe_is_writer || !e->pipe) {
                regs->eax = (uint32_t)-1;
                break;
            }
            pipe_ring_t *r = e->pipe;
            for (;;) {
                uint32_t avail = r->head - r->tail;
                if (avail > 0) {
                    if (avail > len) avail = len;
                    for (uint32_t i = 0; i < avail; i++) {
                        buf[i] = (char)r->buf[(r->tail + i) % PIPE_RING_CAP];
                    }
                    r->tail += avail;
                    regs->eax = avail;
                    break;
                }
                if (r->refcount_w == 0) {
                    regs->eax = 0;          /* EOF: all writers closed */
                    break;
                }
                if (e->flags & FD_FLAG_NONBLOCK) {
                    regs->eax = (uint32_t)-11;  /* -EAGAIN */
                    break;
                }
                task_yield();
            }
        } else {
            regs->eax = (uint32_t)-1;   /* not a readable kind */
        }
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_WRITE(4): write bytes to a file descriptor.
     * EBX = fd, ECX = buf, EDX = len
     * Returns: bytes written (EAX), (uint32_t)-1 on error.
     *
     * VGA:        writes to the VGA/VESA terminal only.
     * VGA_SERIAL: writes to the VGA/VESA terminal AND COM1 (stderr default;
     *             diagnostic output reaches the user's screen and the
     *             captured serial log without an extra syscall).
     * SERIAL:     COM1 only.
     * FILE:       not yet implemented (eager-buffer fd model).
     * ------------------------------------------------------------------ */
    case SYS_WRITE: {
        int         fd  = (int)regs->ebx;
        const char *buf = (const char *)(uintptr_t)regs->ecx;
        uint32_t    len = regs->edx;

        if (!buf) { regs->eax = (uint32_t)-1; break; }

        task_t     *cur = task_current();
        fd_entry_t *e   = fd_get(cur ? cur->fd_table : NULL, fd);
        if (!e) { regs->eax = (uint32_t)-1; break; }

        if (e->kind == FD_KIND_VGA) {
            if (!ktest_muted) {
                for (uint32_t i = 0; i < len; i++)
                    t_putchar(buf[i]);
            }
            regs->eax = len;
        } else if (e->kind == FD_KIND_VGA_SERIAL) {
            if (!ktest_muted) {
                for (uint32_t i = 0; i < len; i++)
                    t_putchar(buf[i]);
            }
            /* t_putchar already mirrors to COM1 when verbose mode is on
             * (default).  Only echo here when verbose is off so stderr
             * always reaches the serial log -- otherwise we'd write the
             * same bytes twice and the log shows every chunk doubled. */
            if (!g_serial_verbose) {
                for (uint32_t i = 0; i < len; i++)
                    Serial_WriteChar(buf[i]);
            }
            regs->eax = len;
        } else if (e->kind == FD_KIND_SERIAL) {
            for (uint32_t i = 0; i < len; i++)
                Serial_WriteChar(buf[i]);
            regs->eax = len;
        } else if (e->kind == FD_KIND_BLOCKDEV) {
            long r = vfs_blockdev_pwrite(e->dev_node, buf, len, e->pos);
            if (r < 0) { regs->eax = (uint32_t)-1; }
            else { e->pos += (uint32_t)r; regs->eax = (uint32_t)r; }
        } else if (e->kind == FD_KIND_FILE) {
            if (!e->writable) { regs->eax = (uint32_t)-1; break; }
            if (e->append) e->pos = e->size;
            /* Refuse writes that would push the buffer past the hard cap. */
            uint64_t want_end = (uint64_t)e->pos + (uint64_t)len;
            if (want_end > SYSCALL_FILE_MAX) {
                regs->eax = (uint32_t)-1;   /* EFBIG */
                break;
            }
            /* Grow geometrically (doubling) so many small TCC-style writes
             * stay amortised O(1).  Floor the first allocation at INITIAL. */
            if (want_end > e->capacity) {
                uint32_t new_cap = e->capacity ? e->capacity : SYSCALL_FILE_INITIAL;
                while ((uint64_t)new_cap < want_end) new_cap <<= 1;
                if (new_cap > SYSCALL_FILE_MAX) new_cap = SYSCALL_FILE_MAX;
                uint8_t *p = (uint8_t *)krealloc(e->data, new_cap);
                if (!p) { regs->eax = (uint32_t)-1; break; }  /* ENOMEM */
                /* Zero the newly-allocated tail so leftover heap garbage
                 * never leaks into ring-3 reads. */
                if (new_cap > e->capacity)
                    memset(p + e->capacity, 0, new_cap - e->capacity);
                e->data     = p;
                e->capacity = new_cap;
            }
            /* lseek-past-EOF: zero-fill the gap between current EOF and pos. */
            if (e->pos > e->size)
                memset(e->data + e->size, 0, e->pos - e->size);
            memcpy(e->data + e->pos, buf, len);
            e->pos += len;
            if (e->pos > e->size) e->size = e->pos;
            e->dirty = 1;
            regs->eax = len;
        } else if (e->kind == FD_KIND_PIPE) {
            /* Writer on a pipe: spin-yield while ring is full.  If every
             * reader closes (refcount_r == 0), writing returns -EPIPE. */
            if (!e->pipe_is_writer || !e->pipe) {
                regs->eax = (uint32_t)-1;
                break;
            }
            pipe_ring_t *r = e->pipe;
            uint32_t written = 0;
            while (written < len) {
                if (r->refcount_r == 0) {
                    /* SIGPIPE not yet implemented; surface as -EPIPE. */
                    regs->eax = written ? written : (uint32_t)-32;
                    break;
                }
                uint32_t inflight = r->head - r->tail;
                uint32_t space    = PIPE_RING_CAP - inflight;
                if (space == 0) {
                    if (e->flags & FD_FLAG_NONBLOCK) {
                        regs->eax = written ? written : (uint32_t)-11;  /* EAGAIN */
                        break;
                    }
                    task_yield();
                    continue;
                }
                uint32_t chunk = len - written;
                if (chunk > space) chunk = space;
                for (uint32_t i = 0; i < chunk; i++) {
                    r->buf[(r->head + i) % PIPE_RING_CAP] = (uint8_t)buf[written + i];
                }
                r->head += chunk;
                written += chunk;
            }
            if (written == len) regs->eax = written;
        } else {
            /* KEYBOARD: not writable. */
            regs->eax = (uint32_t)-1;
        }
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_WRITE_SERIAL(211): write bytes to COM1 serial only (Makar ext).
     * EBX = buf, ECX = len
     * Returns: bytes written (EAX), (uint32_t)-1 on error.
     *
     * Useful for silent telemetry / ktest diagnostics that must not
     * pollute the visible framebuffer. For diagnostic output the user
     * should also see, prefer SYS_WRITE on fd 2 (stderr).
     * ------------------------------------------------------------------ */
    /* ------------------------------------------------------------------
     * SYS_KEYBOARD_RAW(212): enable/disable raw key event delivery
     * for the duration of the current app.  EBX = 0/1.
     *
     * Raw mode suppresses cooked shortcuts (Alt+Fn TTY switch, Ctrl+A
     * pane prefix) and delivers modifier presses + every F-key as
     * sentinel bytes - kbtester is the canonical consumer.  shell_exec_elf
     * defensively forces raw=0 after the child exits in case the app
     * was killed before its own cleanup ran.
     * ------------------------------------------------------------------ */
    case SYS_KEYBOARD_RAW: {
        /* 0 = cooked, 1 = raw sentinels, 2 = scancode passthrough (make+break) */
        int m = (int)regs->ebx;
        keyboard_set_scancode(m == 2);
        keyboard_set_raw(m == 1);
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_SHELL_CLEAR(213): full-screen reset identical to the `clear`
     * shell command.  Calls the same shell_clear_screen() entry point so
     * the VGA colour scheme, VESA pane fg/bg, framebuffer contents, and
     * both cursors all land in the shell's default state.
     *
     * Use this from ring-3 apps that paint custom chrome (kbtester etc.)
     * - sys_tty_clear alone doesn't restore the pane palette, which
     * left the screen looking unchanged when the post-clear background
     * happened to match the app's last cell colour.
     * ------------------------------------------------------------------ */
    case SYS_SHELL_CLEAR:
        shell_clear_screen();
        regs->eax = 0;
        break;

    /* ------------------------------------------------------------------
     * SYS_UPTIME(214): return the kernel tick counter in USER_HZ (100 Hz)
     * units -- stable across the internal PIT rate (see timer.h USER_HZ).
     * Apps that need wall-clock duration (kbtester's hold-Esc, the `clock`
     * widget) can compute (uptime - t0) instead of counting input events
     * whose rate depends on PS/2 typematic settings.
     * ------------------------------------------------------------------ */
    case SYS_UPTIME:
        regs->eax = timer_user_ticks();
        break;

    /* ------------------------------------------------------------------
     * SYS_GETCWD(215): copy the calling task's cwd into a user buffer.
     * EBX = char *buf, ECX = size_t size.
     * Returns: strlen(cwd) on success, (uint32_t)-1 on error (NULL buf,
     * size 0, or buffer too small to hold cwd + NUL).
     * ------------------------------------------------------------------ */
    case SYS_GETCWD: {
        char     *buf  = (char *)(uintptr_t)regs->ebx;
        uint32_t  size = regs->ecx;
        if (!buf || size == 0) { regs->eax = (uint32_t)-1; break; }
        const char *cwd = vfs_getcwd();
        uint32_t    cl  = (uint32_t)strlen(cwd);
        if (cl + 1 > size) { regs->eax = (uint32_t)-1; break; }
        memcpy(buf, cwd, cl + 1);
        regs->eax = cl;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_CHDIR(12): change the calling task's cwd.
     * EBX = const char *path.  Returns 0 on success, -1 on failure
     * (NULL path, missing path, not a directory).  Delegates fully to
     * vfs_cd, which normalises (../, //) and routes through the per-task
     * cwd buffer via task_current().
     * ------------------------------------------------------------------ */
    case SYS_CHDIR: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)(vfs_cd(path) == 0 ? 0 : -1);
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_GETPID(20) / SYS_GETPPID(64): identity accessors.
     * Both fields live on task_t (kernel/task.h:47-48); idle = pid 1,
     * parent_pid 0 means "no parent / spawned by kernel".
     * ------------------------------------------------------------------ */
    case SYS_GETPID: {
        task_t *t = task_current();
        regs->eax = (uint32_t)(t ? t->pid : 0);
        break;
    }
    case SYS_GETPPID: {
        task_t *t = task_current();
        regs->eax = (uint32_t)(t ? t->parent_pid : 0);
        break;
    }

    case SYS_LOGOUT: {
        regs->eax = (vtty_logout_root_session() == 0) ? 0u : (uint32_t)-1;
        break;
    }

    case SYS_LOGIN: {
        const char *user = (const char *)(uintptr_t)regs->ebx;
        const char *pass = (const char *)(uintptr_t)regs->ecx;
        regs->eax = (uint32_t)(auth_login(user, pass) == 0 ? 0 : -1);
        break;
    }

    /* SYS_PASSWD(270): change the current session user's password.  Verify the
     * old password, then write the new one.  Needs a writable (installed)
     * rootfs -- /etc/shadow can't be rewritten on the read-only live CD.
     * Returns 0 ok / -2 wrong current password / -1 otherwise. */
    case SYS_PASSWD: {
        const char *oldp = (const char *)(uintptr_t)regs->ebx;
        const char *newp = (const char *)(uintptr_t)regs->ecx;
        const char *user = auth_current_user();
        if (!oldp || !newp || !user || !user[0] || !vfs_rootfs_is_disk()) {
            regs->eax = (uint32_t)-1; break;
        }
        if (shadow_verify(user, oldp) != 0) { regs->eax = (uint32_t)-2; break; }
        regs->eax = (uint32_t)(shadow_set_password(user, newp) == 0 ? 0 : -1);
        break;
    }

    /* SYS_CAD_PENDING(271): test-and-clear the Ctrl-Alt-Del flag.  The GUI
     * server polls this each frame to open its power menu instantly (the text
     * path only services CAD inside a root text session). */
    case SYS_CAD_PENDING:
        regs->eax = (uint32_t)kb_take_cad_pending();
        break;

    /* SYS_PTY_WINSIZE(272): a GUI terminal publishes its grid size onto a pipe
     * so the piped child's SYS_TERM_SIZE reports it.  EBX=fd, ECX=(cols<<16)|rows. */
    case SYS_PTY_WINSIZE: {
        task_t *cur = task_current();
        fd_entry_t *e = fd_get(cur ? cur->fd_table : NULL, (int)regs->ebx);
        if (e && e->kind == FD_KIND_PIPE && e->pipe) {
            e->pipe->cols = (uint16_t)(regs->ecx >> 16);
            e->pipe->rows = (uint16_t)(regs->ecx & 0xFFFF);
            regs->eax = 0;
        } else regs->eax = (uint32_t)-1;
        break;
    }

    case SYS_GUI_CLOSE: {
        vtty_switch_root_text();
        g_gui_session = 0;          /* Exit to Shell: this session is now CLI */
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_GETTIMEOFDAY(78): write current wall time into struct timeval.
     * EBX = struct timeval *, ECX = struct timezone * (ignored).
     * tv_sec is from the CMOS RTC; tv_usec is approximated from the
     * PIT 100 Hz tick counter (resolution 10 ms, NOT real microseconds).
     * Returns 0 on success, -1 on bad pointer.
     * ------------------------------------------------------------------ */
    case SYS_GETTIMEOFDAY: {
        struct timeval *tv = (struct timeval *)(uintptr_t)regs->ebx;
        if (!tv) { regs->eax = (uint32_t)-1; break; }
        uint32_t secs = 0;
        if (rtc_unix_time(&secs) != 0) { regs->eax = (uint32_t)-1; break; }
        tv->tv_sec  = (int32_t)secs;
        tv->tv_usec = (int32_t)((timer_get_ticks() % TIMER_HZ) * (1000000u / TIMER_HZ));
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_CLOCK_GETTIME(265): write current time into struct timespec.
     * EBX = clockid_t (CLOCK_REALTIME or CLOCK_MONOTONIC), ECX = ts*.
     * REALTIME mirrors SYS_GETTIMEOFDAY; MONOTONIC is timer ticks since
     * boot.  Returns 0 / -1.
     * ------------------------------------------------------------------ */
    case SYS_CLOCK_GETTIME: {
        int clk = (int)regs->ebx;
        struct timespec *ts = (struct timespec *)(uintptr_t)regs->ecx;
        if (!ts) { regs->eax = (uint32_t)-1; break; }
        if (clk == CLOCK_REALTIME) {
            uint32_t secs = 0;
            if (rtc_unix_time(&secs) != 0) { regs->eax = (uint32_t)-1; break; }
            ts->tv_sec  = (int32_t)secs;
            ts->tv_nsec = (int32_t)((timer_get_ticks() % TIMER_HZ) * (1000000000u / TIMER_HZ));
        } else if (clk == CLOCK_MONOTONIC) {
            uint32_t ticks = timer_get_ticks();
            ts->tv_sec  = (int32_t)(ticks / TIMER_HZ);
            ts->tv_nsec = (int32_t)((ticks % TIMER_HZ) * (1000000000u / TIMER_HZ));
        } else {
            regs->eax = (uint32_t)-1;
            break;
        }
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_FB_INFO(216): query framebuffer pixel geometry.
     * Returns (width << 16) | height when VESA is up, 0 when VGA-only.
     * Userspace uses this to pick pixel vs character-cell drawing.
     * ------------------------------------------------------------------ */
    case SYS_FB_INFO: {
        const vesa_fb_t *fb = vesa_get_fb();
        if (!fb || !vesa_tty_is_ready()) { regs->eax = 0; break; }
        uint32_t w = fb->width  & 0xFFFFu;
        uint32_t h = fb->height & 0xFFFFu;
        regs->eax = (w << 16) | h;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DRAW_LINE(217): Bresenham line in framebuffer pixels.
     * EBX = (x0 << 16) | (y0 & 0xFFFF)
     * ECX = (x1 << 16) | (y1 & 0xFFFF)
     * EDX = 24-bit RGB
     *
     * Clipped to [0, fb->width) x [0, drawable_height) where
     * drawable_height excludes the bottom status row -- the same
     * carve-out SYS_TERM_SIZE reports in cell units, just expressed
     * in pixels for graphical apps.  Returns 0 on success, (uint32_t)-1
     * if no pixel framebuffer is available (VGA-only boot).
     * ------------------------------------------------------------------ */
    case SYS_DRAW_LINE: {
        const vesa_fb_t *fb = vesa_get_fb();
        if (!fb || !vesa_tty_is_ready()) { regs->eax = (uint32_t)-1; break; }
        /* Suppress pixel drawing from a backgrounded app so it can't
         * scribble over the visible VT. */
        if (!vtty_is_focused()) {
            task_t *cur = task_current(); if (cur) cur->fb_touched = 1;
            regs->eax = 0; break;
        }
        int32_t x0 = (int32_t)(int16_t)(regs->ebx >> 16);
        int32_t y0 = (int32_t)(int16_t)(regs->ebx & 0xFFFFu);
        int32_t x1 = (int32_t)(int16_t)(regs->ecx >> 16);
        int32_t y1 = (int32_t)(int16_t)(regs->ecx & 0xFFFFu);
        uint32_t rgb = regs->edx & 0xFFFFFFu;

        /* Drawable area excludes the status row when visible.
         * cell_h derives from total FB / total rows so the scale is right;
         * usable_rows() tells us how many content rows to allow. */
        uint32_t total_rows = vesa_tty_get_rows();
        uint32_t cell_h = total_rows ? (fb->height / total_rows) : 0;
        uint32_t usable  = vesa_tty_usable_rows();
        int32_t y_max = (int32_t)(cell_h ? cell_h * usable : fb->height);
        int32_t x_max = (int32_t)fb->width;

        int32_t dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
        int32_t dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
        int32_t sx = (x0 < x1) ? 1 : -1;
        int32_t sy = (y0 < y1) ? 1 : -1;
        int32_t err = dx + dy;
        for (;;) {
            if (x0 >= 0 && x0 < x_max && y0 >= 0 && y0 < y_max)
                vesa_put_pixel((uint32_t)x0, (uint32_t)y0, rgb);
            if (x0 == x1 && y0 == y1) break;
            int32_t e2 = err * 2;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }

        /* Mark fb_touched so the shell's post-exit cleanup wipes the
         * FB and repaints the VT's backing grid.  Without this the
         * stray pixels we just drew would persist under the next
         * prompt. */
        { task_t *cur = task_current(); if (cur) cur->fb_touched = 1; }
        regs->eax = 0;
        break;
    }

    /* SYS_CARET_STYLE(218): set the VESA caret style (0=line, 2=flashing
     * block), returning the previous style so the caller can restore it.
     * No-op returning 0 in VGA-text mode. */
    case SYS_CARET_STYLE: {
        if (!vesa_tty_is_ready()) { regs->eax = 0; break; }
        uint32_t prev = vesa_tty_get_caret_style();
        /* The caret style is a single live-display property; only let the
         * focused VT's task change it, else a backgrounded app (vix's block
         * caret) bleeds onto the visible VT.  Still return prev so callers
         * can save/restore. */
        if (vtty_is_focused())
            vesa_tty_set_caret_style(regs->ebx);
        regs->eax = prev;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_MOUSE_READ(256): pop one PS/2 mouse event (0 when empty).
     * ------------------------------------------------------------------ */
    case SYS_MOUSE_READ:
        regs->eax = vtty_is_focused() ? mouse_pop_event() : 0;
        break;

    /* ------------------------------------------------------------------
     * SYS_FB_PRESENT(257): blit a userspace 32-bpp back buffer (tightly
     * packed, pitch = width*4) full-frame to the framebuffer.  Honours the
     * FB's own pitch.  Gated on focus like SYS_DRAW_LINE so a backgrounded
     * WM can't scribble over the visible VT; sets fb_touched so the VT
     * repaints after the WM exits.  EBX = user back-buffer pointer.
     * ------------------------------------------------------------------ */
    case SYS_FB_PRESENT: {
        const vesa_fb_t *fb = vesa_get_fb();
        if (!fb || !vesa_tty_is_ready()) { regs->eax = (uint32_t)-1; break; }
        if (!vtty_is_focused()) {
            task_t *cur = task_current(); if (cur) cur->fb_touched = 1;
            regs->eax = 0; break;
        }
        const void *src = (const void *)(uintptr_t)regs->ebx;
        video_present_rect(src, 0, 0, fb->width, fb->height);
        { task_t *cur = task_current(); if (cur) cur->fb_touched = 1; }
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_FB_PRESENT_RECT(269): blit only a sub-rectangle of the full-frame
     * user back buffer (same packed-32bpp layout as SYS_FB_PRESENT, pitch =
     * fb->width*4).  EBX = back-buffer base, ECX = (x<<16)|y, EDX = (w<<16)|h.
     * Lets the WM repaint just the cursor's old/new boxes instead of pushing
     * the whole frame on every mouse move.  Clamped to the framebuffer.
     * ------------------------------------------------------------------ */
    case SYS_FB_PRESENT_RECT: {
        const vesa_fb_t *fb = vesa_get_fb();
        if (!fb || !vesa_tty_is_ready()) { regs->eax = (uint32_t)-1; break; }
        if (!vtty_is_focused()) {
            task_t *cur = task_current(); if (cur) cur->fb_touched = 1;
            regs->eax = 0; break;
        }
        uint32_t rx = (regs->ecx >> 16) & 0xFFFFu, ry = regs->ecx & 0xFFFFu;
        uint32_t rw = (regs->edx >> 16) & 0xFFFFu, rh = regs->edx & 0xFFFFu;
        if (rx >= fb->width || ry >= fb->height) { regs->eax = 0; break; }
        const void *src = (const void *)(uintptr_t)regs->ebx;
        video_present_rect(src, rx, ry, rw, rh);
        { task_t *cur = task_current(); if (cur) cur->fb_touched = 1; }
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * Display-driver hooks (kernel/video.h).  video_caps lets the WM learn
     * whether a hardware cursor is available; the hwcursor calls drive the
     * active driver's cursor sprite (the WM uses them instead of compositing
     * a software cursor, so a pure mouse move costs no framebuffer traffic).
     * ------------------------------------------------------------------ */
    case SYS_VIDEO_CAPS:
        regs->eax = video_caps();
        break;
    case SYS_HWCURSOR_DEFINE: {
        const uint32_t *argb = (const uint32_t *)(uintptr_t)regs->ebx;
        int w  = (int)((regs->ecx >> 16) & 0xFFFFu), h  = (int)(regs->ecx & 0xFFFFu);
        int hx = (int)((regs->edx >> 16) & 0xFFFFu), hy = (int)(regs->edx & 0xFFFFu);
        const vid_driver_t *d = video_active();
        regs->eax = (d && d->cursor_define)
                        ? (uint32_t)d->cursor_define(argb, w, h, hx, hy)
                        : (uint32_t)-1;
        break;
    }
    case SYS_HWCURSOR_MOVE: {
        const vid_driver_t *d = video_active();
        if (d && d->cursor_move) d->cursor_move((int)regs->ebx, (int)regs->ecx);
        regs->eax = 0;
        break;
    }
    case SYS_HWCURSOR_SHOW: {
        const vid_driver_t *d = video_active();
        if (d && d->cursor_show) d->cursor_show((int)regs->ebx);
        regs->eax = 0;
        break;
    }

    case SYS_WRITE_SERIAL: {
        const char *buf = (const char *)(uintptr_t)regs->ebx;
        uint32_t    len = regs->ecx;

        if (!buf) { regs->eax = (uint32_t)-1; break; }

        for (uint32_t i = 0; i < len; i++)
            Serial_WriteChar(buf[i]);
        regs->eax = len;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_OPEN(5): open a VFS path and return a file descriptor.
     * EBX = path (NUL-terminated string in user space)
     * ECX = flags (O_RDONLY=0; other modes not yet supported)
     * Returns: fd on success, (uint32_t)-1 on error.
     *
     * The entire file is read into a heap buffer (cap: SYSCALL_FILE_MAX).
     * Allocated in the calling task's per-task fd table.
     * ------------------------------------------------------------------ */
    case SYS_OPEN: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        uint32_t    flags = regs->ecx;

        if (!path) { regs->eax = (uint32_t)-1; break; }
        /* Reject paths that wouldn't fit in fd_entry_t.path[] -- otherwise
         * close-flush would write back to the wrong (truncated) path. */
        {
            uint32_t n = 0;
            while (n < VFS_PATH_MAX && path[n]) n++;
            if (n >= VFS_PATH_MAX) { regs->eax = (uint32_t)-1; break; }
        }

        task_t *cur = task_current();
        if (!cur || !cur->fd_table) { regs->eax = (uint32_t)-1; break; }

        int fd = fd_alloc(cur->fd_table);
        if (fd < 0) { regs->eax = (uint32_t)-1; break; }  /* EMFILE */

        /* Block devices under /dev are not eager-buffered. */
        uint32_t dev_sz = 0;
        int dev_node = vfs_blockdev_lookup(path, &dev_sz);
        if (dev_node >= 0) {
            fd_entry_t *de = &cur->fd_table->slots[fd];
            memset(de, 0, sizeof(*de));
            de->kind     = FD_KIND_BLOCKDEV;
            de->size     = dev_sz;
            de->dev_node = dev_node;
            regs->eax = (uint32_t)fd;
            break;
        }

        int acc      = (int)(flags & O_ACCMODE);
        int writable = (acc != O_RDONLY);
        int o_creat  = (flags & O_CREAT)  != 0;
        int o_trunc  = (flags & O_TRUNC)  != 0;
        int o_append = (flags & O_APPEND) != 0;
        int exists   = vfs_file_exists(path);

        fd_entry_t *e = &cur->fd_table->slots[fd];
        memset(e, 0, sizeof(*e));

        if (!exists) {
            if (!o_creat) { regs->eax = (uint32_t)-1; break; }
            /* Create: allocate an empty growable buffer and mark dirty
             * so close flushes (even if no writes follow) -- this is
             * what makes `touch`-style "create empty file" work. */
            uint8_t *buf = (uint8_t *)kmalloc(SYSCALL_FILE_INITIAL);
            if (!buf) { regs->eax = (uint32_t)-1; break; }
            memset(buf, 0, SYSCALL_FILE_INITIAL);
            e->kind     = FD_KIND_FILE;
            e->data     = buf;
            e->size     = 0;
            e->capacity = SYSCALL_FILE_INITIAL;
            e->dirty    = 1;
        } else if (writable && o_trunc) {
            /* Truncate-on-open: skip the eager-load and start empty. */
            uint8_t *buf = (uint8_t *)kmalloc(SYSCALL_FILE_INITIAL);
            if (!buf) { regs->eax = (uint32_t)-1; break; }
            memset(buf, 0, SYSCALL_FILE_INITIAL);
            e->kind     = FD_KIND_FILE;
            e->data     = buf;
            e->size     = 0;
            e->capacity = SYSCALL_FILE_INITIAL;
            e->dirty    = 1;
        } else {
            vfs_stat_info_t si;
            int have_stat = (vfs_stat(path, &si) == 0);

            /* Lazy read-only path (WWLD): a read-only open on a seekable disk
             * backend is *not* eager-loaded.  We record the size and stream the
             * file a page at a time through the kernel page cache on each read
             * (see SYS_READ) -- so opening a 29 MiB WAD costs ~0 heap instead of
             * a 29 MiB kmalloc.  Writable opens and synthetic backends
             * (procfs/tmpfs/...) keep the buffered path below. */
            if (have_stat && !writable && si.kind == VFS_STAT_FILE &&
                vfs_path_is_disk(path)) {
                e->kind     = FD_KIND_FILE;
                e->data     = NULL;
                e->capacity = 0;
                e->size     = si.size;
                e->lazy     = 1;
            } else {
                /* Buffered: size the allocation to the actual file (probed via
                 * vfs_stat) so a tiny file doesn't kmalloc SYSCALL_FILE_MAX.
                 * Writable fds round up to SYSCALL_FILE_INITIAL so the first
                 * write needn't grow immediately. */
                uint32_t cap;
                if (have_stat) {
                    cap = si.size;
                    if (writable && cap < SYSCALL_FILE_INITIAL) cap = SYSCALL_FILE_INITIAL;
                    if (cap == 0) cap = SYSCALL_FILE_INITIAL;   /* empty file: 1 page */
                    if (cap > SYSCALL_FILE_MAX) cap = SYSCALL_FILE_MAX;
                } else {
                    /* vfs_stat unsupported on this backend; fall back to the
                     * conservative large allocation. */
                    cap = SYSCALL_FILE_MAX;
                }
                uint8_t *buf = (uint8_t *)kmalloc(cap);
                if (!buf) { regs->eax = (uint32_t)-1; break; }
                uint32_t out_sz = 0;
                if (vfs_read_file(path, buf, cap, &out_sz) != 0) {
                    kfree(buf);
                    regs->eax = (uint32_t)-1;
                    break;
                }
                e->kind     = FD_KIND_FILE;
                e->data     = buf;
                e->size     = out_sz;
                e->capacity = cap;
            }
        }

        e->writable = writable ? 1 : 0;
        e->append   = o_append ? 1 : 0;
        /* Inline-copy the path for close-flush. */
        {
            uint32_t n = 0;
            while (n < VFS_PATH_MAX - 1 && path[n]) { e->path[n] = path[n]; n++; }
            e->path[n] = '\0';
        }
        e->pos = o_append ? e->size : 0;

        regs->eax = (uint32_t)fd;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_CLOSE(6): close a file descriptor and free its buffer.
     * EBX = fd
     * Returns: 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_CLOSE: {
        int     fd  = (int)regs->ebx;
        task_t *cur = task_current();
        regs->eax = (fd_close(cur ? cur->fd_table : NULL, fd) == 0)
                        ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_PIPE(42): create a unidirectional pipe.
     * EBX = int pipefd[2] (out)  -- pipefd[0]=read end, pipefd[1]=write end
     * Returns: 0 on success, -1 on error.
     *
     * Both ends point at the same 4 KiB pipe_ring_t; refcounted so
     * fork+dup2 can share the buffer until the last fd closes.
     * ------------------------------------------------------------------ */
    case SYS_PIPE: {
        int *pipefd = (int *)(uintptr_t)regs->ebx;
        task_t *cur = task_current();
        fd_table_t *tbl = cur ? cur->fd_table : NULL;
        if (!pipefd || !tbl) { regs->eax = (uint32_t)-1; break; }
        int rfd = fd_alloc(tbl);
        if (rfd < 0) { regs->eax = (uint32_t)-1; break; }
        /* Mark the reader slot allocated (so fd_alloc finds a different
         * slot for the writer).  Set kind to NONE briefly is wrong;
         * instead temporarily install the kind, then fix up after alloc. */
        fd_entry_t *re = &tbl->slots[rfd];
        re->kind = FD_KIND_PIPE;            /* reserve slot */
        int wfd = fd_alloc(tbl);
        if (wfd < 0) {
            memset(re, 0, sizeof(*re));
            regs->eax = (uint32_t)-1;
            break;
        }
        fd_entry_t *we = &tbl->slots[wfd];
        we->kind = FD_KIND_PIPE;
        pipe_ring_t *ring = (pipe_ring_t *)kmalloc(sizeof(*ring));
        if (!ring) {
            memset(re, 0, sizeof(*re));
            memset(we, 0, sizeof(*we));
            regs->eax = (uint32_t)-1;
            break;
        }
        ring->head = ring->tail = 0;
        ring->refcount_r = 1;
        ring->refcount_w = 1;
        re->pipe = ring;
        re->pipe_is_writer = 0;
        we->pipe = ring;
        we->pipe_is_writer = 1;
        pipefd[0] = rfd;
        pipefd[1] = wfd;
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DUP(41): duplicate oldfd onto the lowest-numbered free fd.
     * EBX = oldfd
     * Returns: new fd on success, -1 on error (bad oldfd / table full).
     *
     * Same shallow-copy semantics as SYS_DUP2 below (FD_KIND_PIPE shares
     * the ring via refcount bump; FILE slots alias their buffer pointer).
     * ------------------------------------------------------------------ */
    case SYS_DUP: {
        int oldfd = (int)regs->ebx;
        task_t *cur = task_current();
        fd_table_t *tbl = cur ? cur->fd_table : NULL;
        if (!tbl) { regs->eax = (uint32_t)-1; break; }
        fd_entry_t *oe = fd_get(tbl, oldfd);
        if (!oe) { regs->eax = (uint32_t)-1; break; }
        int newfd = fd_alloc(tbl);
        if (newfd < 0) { regs->eax = (uint32_t)-1; break; }
        fd_entry_t *ne = &tbl->slots[newfd];
        memcpy(ne, oe, sizeof(*ne));
        if (oe->kind == FD_KIND_PIPE && ne->pipe) {
            if (ne->pipe_is_writer) ne->pipe->refcount_w++;
            else                    ne->pipe->refcount_r++;
        }
        regs->eax = (uint32_t)newfd;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DUP2(63): duplicate oldfd onto newfd, closing newfd first.
     * EBX = oldfd, ECX = newfd
     * Returns: newfd on success, -1 on error.
     *
     * For FD_KIND_PIPE the underlying ring is shared (refcount bump);
     * other kinds shallow-copy the slot, with FILE slots intentionally
     * NOT deep-copying their buffer -- dup2 of a FILE-kind fd today
     * aliases the buffer pointer, which is undefined behaviour for
     * concurrent writes but matches the "no real open_file_t" stance.
     * ------------------------------------------------------------------ */
    case SYS_DUP2: {
        int oldfd = (int)regs->ebx;
        int newfd = (int)regs->ecx;
        task_t *cur = task_current();
        fd_table_t *tbl = cur ? cur->fd_table : NULL;
        if (!tbl || newfd < 0 || newfd >= TASK_MAX_FDS) {
            regs->eax = (uint32_t)-1; break;
        }
        fd_entry_t *oe = fd_get(tbl, oldfd);
        if (!oe) { regs->eax = (uint32_t)-1; break; }
        if (oldfd == newfd) { regs->eax = (uint32_t)newfd; break; }
        /* Close target if currently open (ignore close errors -- POSIX). */
        if (tbl->slots[newfd].kind != FD_KIND_NONE) {
            (void)fd_close(tbl, newfd);
        }
        fd_entry_t *ne = &tbl->slots[newfd];
        memcpy(ne, oe, sizeof(*ne));
        if (oe->kind == FD_KIND_PIPE && ne->pipe) {
            if (ne->pipe_is_writer) ne->pipe->refcount_w++;
            else                    ne->pipe->refcount_r++;
        }
        regs->eax = (uint32_t)newfd;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_BRK(45): set or query the user-space heap break.
     * EBX = requested new break (0 = query current break)
     * Returns: current break after the call.
     *
     * Pages are allocated from the PMM and mapped writable+user in the
     * current task's page directory.  The break only ever grows.
     * ------------------------------------------------------------------ */
    case SYS_BRK: {
        uint32_t  new_brk = regs->ebx;
        task_t   *t       = task_current();

        if (new_brk == 0 || new_brk <= t->user_brk) {
            regs->eax = t->user_brk;
            break;
        }

        /* Demand-paged heap (WWLD: Linux brk only reserves; pages are mapped
         * lazily on first touch).  Just advance the break -- the page-fault
         * handler maps a zeroed frame for any access in [user_brk_base,
         * user_brk).  This keeps a multi-MiB growth (e.g. doom's 6 MiB zone)
         * from stalling every other task in one interrupts-off syscall. */
        if (t->user_brk_base == 0) t->user_brk_base = t->user_brk;
        t->user_brk = new_brk;
        regs->eax   = new_brk;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_MMAP2(192): anonymous mmap only.  Linux i386 ABI; args:
     *   EBX=addr (hint, ignored unless future MAP_FIXED), ECX=len,
     *   EDX=prot, ESI=flags, (EDI=fd, EBP=pgoff -- ignored for anon).
     * Allocates ceil(len/4KiB) zeroed frames into a per-task bump window
     * [USER_MMAP_BASE, stack) and returns the base.  File-backed mmap and
     * MAP_FIXED are unsupported -> MAP_FAILED ((void*)-1).  This is what
     * a hosted malloc (musl mallocng) needs beyond brk.
     * ------------------------------------------------------------------ */
    case SYS_MMAP2: {
        #define MMAP_MAP_ANONYMOUS 0x20u
        #define MMAP_MAP_FIXED     0x10u
        uint32_t len   = regs->ecx;
        uint32_t flags = regs->esi;
        task_t  *t     = task_current();

        if (!t || len == 0 || !(flags & MMAP_MAP_ANONYMOUS) ||
            (flags & MMAP_MAP_FIXED)) {
            regs->eax = (uint32_t)-1; break;          /* MAP_FAILED */
        }

        uint32_t pages = (len + 0xFFFu) >> 12;
        if (t->mmap_next == 0) t->mmap_next = USER_MMAP_BASE;
        uint32_t base = t->mmap_next;

        /* Don't collide with the ring-3 stack region. */
        if (base + (pages << 12) >= 0xBFFF0000u - (8u * 0x1000u)) {
            regs->eax = (uint32_t)-1; break;
        }

        /* Demand-paged anonymous mmap: reserve the window only; the page-fault
         * handler maps a zeroed frame on first touch in [USER_MMAP_BASE,
         * mmap_next).  Same WWLD lazy model as brk above. */
        t->mmap_next = base + (pages << 12);
        regs->eax = base;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_MUNMAP(91): unmap + free a range previously returned by mmap2.
     * EBX=addr, ECX=len.  No address reuse (mmap_next never rewinds) --
     * fine for bring-up.  Returns 0 (we don't validate the range).
     * ------------------------------------------------------------------ */
    case SYS_MUNMAP: {
        uint32_t addr = regs->ebx & ~0xFFFu;
        uint32_t len  = regs->ecx;
        task_t  *t    = task_current();
        if (t && len) {
            uint32_t pages = (len + 0xFFFu) >> 12;
            for (uint32_t i = 0; i < pages; i++)
                vmm_unmap_page(t->page_dir, addr + (i << 12));
        }
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * musl/Linux process-startup stubs.  Enough for a static-musl binary's
     * __init_libc to get through start-up; not full implementations.
     * ------------------------------------------------------------------ */
    case SYS_EXIT_GROUP: {           /* exit_group == exit for our 1-thread procs */
        task_t *t = task_current();
        if (t) t->exit_status = (int)regs->ebx;
        task_exit();                 /* does not return */
        break;
    }
    case SYS_SET_TID_ADDRESS: {       /* musl stores clear_child_tid; just give a tid */
        task_t *t = task_current();
        regs->eax = (uint32_t)(t ? t->pid : 1);
        break;
    }
    case SYS_RT_SIGPROCMASK:          /* no real signal mask plumbing yet */
        regs->eax = 0;
        break;
    case SYS_IOCTL:                   /* isatty() probes this; report not-a-tty */
        regs->eax = (uint32_t)(-25);  /* -ENOTTY */
        break;
    case SYS_FUTEX:                   /* single-threaded: locks never contend */
        regs->eax = 0;
        break;

    /* ------------------------------------------------------------------
     * SYS_SET_THREAD_AREA(243): install the calling task's TLS segment.
     * EBX = struct user_desc* { entry_number, base_addr, limit, flags }.
     * entry_number == -1 -> use the one TLS GDT slot (index 6) and write the
     * index back.  Sets task->tls_gs so the scheduler restores it on switch.
     * ------------------------------------------------------------------ */
    case SYS_SET_THREAD_AREA: {
        uint32_t *u = (uint32_t *)(uintptr_t)regs->ebx;
        task_t   *t = task_current();
        if (!u || !t) { regs->eax = (uint32_t)-1; break; }

        uint32_t entry = u[0];
        uint32_t base  = u[1];
        uint32_t limit = u[2];
        uint32_t flags = u[3];
        int limit_in_pages = (int)((flags >> 4) & 1u);
        int present        = !((flags >> 5) & 1u);   /* seg_not_present inverted */

        if (entry != 0xFFFFFFFFu && entry != (uint32_t)GDT_TLS_INDEX) {
            regs->eax = (uint32_t)-1; break;          /* only one TLS slot */
        }

        int idx = gdt_set_tls(base, limit, limit_in_pages, present);
        u[0] = (uint32_t)idx;                         /* write back entry_number */

        t->tls_base   = base;
        t->tls_limit  = limit;
        t->tls_pages  = (uint8_t)limit_in_pages;
        t->tls_active = 1;
        t->tls_gs     = ((uint32_t)idx << 3) | 3u;    /* selector 0x33 */

        { uint16_t sel = (uint16_t)t->tls_gs;
          __asm__ volatile("movw %0, %%gs" :: "r"(sel)); }

        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_YIELD(158): voluntarily give up the CPU.
     * ------------------------------------------------------------------ */
    case SYS_YIELD:
        /* Flush any pending VT-switch repaint here too.  A fullscreen app
         * (basic/lines, maktop, ...) yields but never calls keyboard_getchar
         * in its draw loop, so without this the status bar wouldn't follow
         * a switch back to the app's VT. */
        vtty_drain_pending();
        task_yield();
        break;

    /* ------------------------------------------------------------------
     * SYS_DEBUG(100): Makar extension - print a debug checkpoint.
     * EBX = uint32 checkpoint value, written to VGA + serial.
     * ------------------------------------------------------------------ */
    case SYS_DEBUG:
        g_ring3_last_cp = regs->ebx;
        Serial_WriteString("[ring3] CP: 0x");
        Serial_WriteHex(regs->ebx);
        Serial_WriteString("\n");
        if (!ktest_muted) {
            t_writestring("[ring3] CP: 0x");
            t_hex(regs->ebx);
            t_putchar('\n');
        }
        break;

    /* ------------------------------------------------------------------
     * SYS_LSEEK(19): seek within an open file fd.
     * EBX = fd, ECX = offset, EDX = whence (0=SET,1=CUR,2=END)
     * ------------------------------------------------------------------ */
    case SYS_LSEEK: {
        int     fd     = (int)regs->ebx;
        int     offset = (int)regs->ecx;
        int     whence = (int)regs->edx;
        task_t *cur    = task_current();
        fd_entry_t *e  = fd_get(cur ? cur->fd_table : NULL, fd);
        if (!e || (e->kind != FD_KIND_FILE && e->kind != FD_KIND_BLOCKDEV)) {
            regs->eax = (uint32_t)-1; break;
        }
        uint32_t new_pos;
        if (whence == 0)      new_pos = (uint32_t)offset;
        else if (whence == 1) new_pos = (uint32_t)((int)e->pos + offset);
        else if (whence == 2) new_pos = (uint32_t)((int)e->size + offset);
        else { regs->eax = (uint32_t)-1; break; }
        /* Writable FILE fds allow seek-past-EOF (SYS_WRITE will zero-fill
         * the gap on the next write).  Read-only fds and block devices
         * still clamp at e->size as before. */
        if (e->kind == FD_KIND_FILE && e->writable) {
            if (new_pos > SYSCALL_FILE_MAX) new_pos = SYSCALL_FILE_MAX;
        } else {
            if (new_pos > e->size) new_pos = e->size;
        }
        e->pos = new_pos;
        regs->eax = new_pos;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_STAT(106) / SYS_FSTAT(108): fill a Linux i386 struct stat.
     * Only st_mode / st_size / st_ino / st_blksize / st_nlink are
     * populated; the rest are zero-filled.  st_ino is the FNV-1a-32
     * hash of the resolved path (stable per boot, sufficient for TCC).
     * ------------------------------------------------------------------ */
    case SYS_STAT: {
        const char  *upath = (const char *)(uintptr_t)regs->ebx;
        struct stat *ust   = (struct stat *)(uintptr_t)regs->ecx;
        if (!upath || !ust) { regs->eax = (uint32_t)-1; break; }
        vfs_stat_info_t si;
        if (vfs_stat(upath, &si) != 0) { regs->eax = (uint32_t)-1; break; }
        struct stat st; memset(&st, 0, sizeof st);
        /* FNV-1a 32-bit over the (user-supplied, unresolved) path. */
        uint32_t h = 2166136261u;
        for (const char *p = upath; *p; p++) {
            h ^= (uint8_t)*p; h *= 16777619u;
        }
        st.st_ino     = h;
        st.st_nlink   = 1;
        st.st_blksize = 4096;
        st.st_size    = si.size;
        if      (si.kind == VFS_STAT_DIR)      st.st_mode = S_IFDIR | 0755;
        else if (si.kind == VFS_STAT_BLOCKDEV) st.st_mode = S_IFBLK | 0644;
        else if (si.kind == VFS_STAT_CHARDEV)  st.st_mode = S_IFCHR | 0644;
        else                                   st.st_mode = S_IFREG | 0644;
        memcpy(ust, &st, sizeof st);
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_READDIR(141): index-addressed directory enumeration.
     * EBX = path, ECX = index, EDX = struct dirent *.
     * Returns 1 if filled, 0 if index past end, -1 on error.
     * Wraps vfs_complete; bounded by an internal collector since vfs_complete
     * uses callback-per-entry rather than streaming -- fine for the
     * modest directory sizes Makar handles today.
     * ------------------------------------------------------------------ */
    case SYS_READDIR: {
        const char    *path = (const char *)(uintptr_t)regs->ebx;
        uint32_t       idx  = regs->ecx;
        struct dirent *ude  = (struct dirent *)(uintptr_t)regs->edx;
        if (!path || !ude) { regs->eax = (uint32_t)-1; break; }
        struct rd_ctx ctx = { idx, 0, 0, {0}, 0 };
        if (vfs_complete(path, "", readdir_collect_cb, &ctx) != 0) {
            regs->eax = (uint32_t)-1; break;
        }
        if (!ctx.found) { regs->eax = 0; break; }
        memset(ude, 0, sizeof(*ude));
        uint32_t h = 2166136261u;
        for (const char *q = ctx.name; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
        ude->d_ino  = h;
        ude->d_type = ctx.is_dir ? DT_DIR : DT_REG;
        uint32_t i = 0;
        while (ctx.name[i] && i < DIRENT_NAME_MAX - 1) { ude->d_name[i] = ctx.name[i]; i++; }
        ude->d_name[i] = '\0';
        regs->eax = 1;
        break;
    }

    case SYS_FSTAT: {
        int          fd  = (int)regs->ebx;
        struct stat *ust = (struct stat *)(uintptr_t)regs->ecx;
        if (!ust) { regs->eax = (uint32_t)-1; break; }
        task_t *cur = task_current();
        fd_entry_t *e = fd_get(cur ? cur->fd_table : NULL, fd);
        if (!e) { regs->eax = (uint32_t)-1; break; }
        struct stat st; memset(&st, 0, sizeof st);
        st.st_nlink   = 1;
        st.st_blksize = 4096;
        if (e->kind == FD_KIND_FILE) {
            st.st_mode = S_IFREG | 0644;
            st.st_size = e->size;
            uint32_t h = 2166136261u;
            for (const char *p = e->path; *p; p++) {
                h ^= (uint8_t)*p; h *= 16777619u;
            }
            st.st_ino = h;
        } else if (e->kind == FD_KIND_BLOCKDEV) {
            st.st_mode = S_IFBLK | 0644;
            st.st_size = e->size;
        } else {
            st.st_mode = S_IFCHR | 0644;
        }
        memcpy(ust, &st, sizeof st);
        regs->eax = 0;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_GETKEY(200): raw single-char keyboard read - no echo, no
     * line-buffering.  Returns raw char value including arrow sentinels
     * (0x80-0x83) as unsigned bytes in EAX.
     * ------------------------------------------------------------------ */
    case SYS_GETKEY: {
        int pc = stdin_pipe_getchar(task_current());
        regs->eax = (pc != -2) ? (uint32_t)(int32_t)pc
                               : (uint32_t)(uint8_t)keyboard_getchar();
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_PUTCH_AT(201): write an array of screen cells.
     * EBX = pointer to tty_cell_t[], ECX = count.
     * ------------------------------------------------------------------ */
    case SYS_PUTCH_AT: {
        const tty_cell_t *cells = (const tty_cell_t *)(uintptr_t)regs->ebx;
        uint32_t n = regs->ecx;
        if (!cells || n == 0) { regs->eax = 0; break; }

        /* GUI terminal: no live VT + piped stdout -> emit ANSI for mxterm. */
        { fd_entry_t *abr = ansi_bridge_fd();
          if (abr) {
              char out[64]; int lr = -1, lc = -2, lclr = -1;
              for (uint32_t i = 0; i < n; i++) {
                  int row = cells[i].row, col = cells[i].col;
                  int clr = cells[i].clr; char ch = (char)cells[i].ch;
                  if (row != lr || col != lc + 1) {        /* reposition */
                      int o = ansi_cup(out, (unsigned)row, (unsigned)col);
                      ansi_pipe_write(abr, out, (uint32_t)o); lclr = -1;
                  }
                  if (clr != lclr) {
                      int o = ansi_sgr(out, (unsigned char)clr);
                      ansi_pipe_write(abr, out, (uint32_t)o); lclr = clr;
                  }
                  ansi_pipe_write(abr, &ch, 1);
                  lr = row; lc = col;
              }
              { char rst[3] = { 0x1b, '[', 'm' }; ansi_pipe_write(abr, rst, 3); }
              regs->eax = 0; break;
          } }
        /* Mark this task as "touched the framebuffer".  shell_exec_elf
         * inspects this flag after the child dies and reissues
         * shell_clear_screen if set, so fullscreen apps that exit via
         * SIGKILL (no chance to clean up) don't leave their last frame
         * underneath the next shell prompt. */
        { task_t *cur = task_current(); if (cur) cur->fb_touched = 1; }

        /* Always record cells into the calling task's VT backing grid so
         * the compositor can repaint the app's frame when the operator
         * Alt+Fn's back to it.  Only paint the live framebuffer when the
         * task is on the focused VT -- otherwise a backgrounded fullscreen
         * app (e.g. maktop on VT2 while VT1 is visible) would bleed its
         * cells onto whatever VT is currently shown. */
        vt_buf_t *vt      = vtty_buf_current();
        int       focused = vt ? vtty_is_focused() : 1;

        /* SYS_PUTCH_AT cells carry their own colour attribute, so writing
         * each cell mutates the default pane's fg/bg.  Save the pane
         * colours up-front and restore at the end so apps that paint
         * coloured chrome (kbtester, future status bars) don't leave the
         * shell stuck in their palette after exit. */
        /* Always fetch dp so is_status can be computed even for unfocused
         * tasks (e.g. makmux parent on VTTY_ROOT_SLOT drawing the status
         * bar while mak.sh1 is the focused VT).  The save/restore below
         * also needs dp to prevent amber status-bar colours from bleeding
         * into the focused task's palette. */
        vesa_pane_t *dp = vesa_tty_is_ready() ? vesa_tty_default_pane() : NULL;
        uint32_t saved_fg = dp ? dp->fg : 0;
        uint32_t saved_bg = dp ? dp->bg : 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t col = cells[i].col;
            uint8_t row = cells[i].row;
            uint8_t ch  = cells[i].ch;
            uint8_t clr = cells[i].clr;
            uint32_t fg = s_vga_palette[clr & 0x0F];
            uint32_t bg = s_vga_palette[(clr >> 4) & 0x0F];

            int is_status = dp && (uint32_t)row >= dp->rows;

            /* Drawable-area cells: write into the VT buffer and render to
             * the framebuffer when focused.  Skip vt_set_color/vt_put_at
             * for status-bar rows — they must not overwrite vt->fg/bg with
             * the status-bar palette (which would corrupt mak.sh0's colours
             * on repaint) and vt_put_at silently ignores out-of-range rows
             * anyway. */
            if (vt && !is_status) {
                vt_set_color(vt, fg, bg);
                vt_put_at(vt, (char)ch, col, row);
            }

            /* Framebuffer write:
             *  - drawable rows  → only when focused (normal VT isolation)
             *  - status-bar row → always; makmux owns the bottom row
             *    regardless of which VT is active or focused. */
            if (focused || is_status) {
                t_putentryat((char)ch, clr, col, row);
                if (dp) {
                    /* vesa_tty_paint_cell takes fg/bg directly; vesa_tty_put_at
                     * reads vt->fg/bg which vt_set_color already set above.
                     * Do NOT call vesa_tty_setcolor here — it would pollute
                     * default_pane and the calling task's VT buffer fg/bg with
                     * whatever colours the current cell carries (status-bar
                     * amber, etc.), corrupting subsequent output. */
                    if (is_status)
                        vesa_tty_paint_cell(col, row, (char)ch, fg, bg);
                    else
                        vesa_tty_put_at((char)ch, col, row);
                }
            }
        }
        if (dp) {
            dp->fg = saved_fg;
            dp->bg = saved_bg;
        }
        regs->eax = n;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_SET_CURSOR(202): move cursor.
     * EBX = col, ECX = row.
     * ------------------------------------------------------------------ */
    case SYS_SET_CURSOR: {
        { fd_entry_t *abr = ansi_bridge_fd();
          if (abr) { char out[24]; int o = ansi_cup(out, regs->ecx, regs->ebx);
                     ansi_pipe_write(abr, out, (uint32_t)o); break; } }
        /* Record into the backing grid so the cursor lands correctly on
         * repaint; only move the visible hardware cursor when focused. */
        vt_buf_t *vt = vtty_buf_current();
        if (vt) vt_set_cursor(vt, regs->ebx, regs->ecx);
        if (!vt || vtty_is_focused())
            t_set_cursor((size_t)regs->ebx, (size_t)regs->ecx);
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_TTY_CLEAR(203): fill screen with spaces.
     * EBX = VGA colour attribute (e.g. 0x07 = white-on-black).
     * ------------------------------------------------------------------ */
    case SYS_TTY_CLEAR: {
        { fd_entry_t *abr = ansi_bridge_fd();
          if (abr) { char out[24]; int o = ansi_sgr(out, (unsigned char)regs->ebx);
                     ansi_pipe_write(abr, out, (uint32_t)o);
                     const char cl[] = { 0x1b,'[','2','J', 0x1b,'[','H' };
                     ansi_pipe_write(abr, cl, 7); break; } }
        { task_t *cur = task_current(); if (cur) cur->fb_touched = 1; }
        /* Clear the backing grid to the requested attribute always; wipe
         * the live screen only when focused. */
        uint8_t clr  = (uint8_t)regs->ebx;
        vt_buf_t *vt = vtty_buf_current();
        if (vt) {
            vt_set_color(vt, s_vga_palette[clr & 0x0F],
                             s_vga_palette[(clr >> 4) & 0x0F]);
            vt_clear(vt);
        }
        if (!vt || vtty_is_focused())
            t_fill(clr);
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_TERM_SIZE(204): query terminal dimensions.
     * Returns EAX = (cols << 16) | rows.
     * ------------------------------------------------------------------ */
    case SYS_TERM_SIZE: {
        /* GUI terminal child: report the terminal's published pty size. */
        { fd_entry_t *abr = ansi_bridge_fd();
          if (abr && abr->pipe->cols && abr->pipe->rows) {
              regs->eax = ((uint32_t)abr->pipe->cols << 16) | abr->pipe->rows;
              break;
          } }
        /* Report the *drawable* area (vesa_tty_usable_rows), which excludes
         * the bottom status row whenever the status bar is enabled (reserved)
         * -- so shells/apps stay above it and statusbar.elf draws at row =
         * usable_rows.  When the bar is toggled off (Alt+F5) the row is freed
         * and apps get the full height.  Resolution-aware. */
        uint32_t cols, rows;
        if (vesa_tty_is_ready()) {
            cols = vesa_tty_get_cols();
            rows = vesa_tty_usable_rows();
        } else {
            cols = VGA_WIDTH;
            rows = (uint32_t)t_get_rows();
        }
        regs->eax = (cols << 16) | rows;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_WRITE_FILE(205): create or overwrite a VFS file.
     * EBX = path, ECX = buf, EDX = len.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_WRITE_FILE: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        const void *buf  = (const void *)(uintptr_t)regs->ecx;
        uint32_t    len  = regs->edx;
        if (!path || !buf) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_write_file(path, buf, len) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_LS_DIR(206): list a VFS directory into a text buffer.
     * EBX = path, ECX = buf, EDX = bufsz.
     * Returns bytes written.
     * ------------------------------------------------------------------ */
    case SYS_LS_DIR: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        char *buf        = (char *)(uintptr_t)regs->ecx;
        uint32_t bufsz   = regs->edx;
        if (!path || !buf || bufsz == 0) { regs->eax = 0; break; }
        ls_ctx_t ctx = { buf, bufsz, 0 };
        buf[0] = '\0';
        vfs_complete(path, "", ls_cb, &ctx);
        regs->eax = ctx.off;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DISK_INFO(207): write detected drive info as text.
     * EBX = buf, ECX = bufsz.
     * Returns bytes written.
     * ------------------------------------------------------------------ */
    case SYS_DISK_INFO: {
        char    *buf   = (char *)(uintptr_t)regs->ebx;
        uint32_t cap   = regs->ecx;
        if (!buf || cap == 0) { regs->eax = 0; break; }
        uint32_t off = 0;
        for (int i = 0; i < IDE_MAX_DRIVES; i++) {
            const ide_drive_t *d = ide_get_drive((uint8_t)i);
            if (!d || !d->present) continue;
            /* "drive N: TYPE size_sectors sectors\n" */
            const char *type = (d->type == IDE_TYPE_ATAPI) ? "ATAPI" : "ATA";
            const char *parts[] = { "drive ", NULL, ": ", type, " " };
            char num[4];
            num[0] = (char)('0' + i); num[1] = '\0';
            parts[1] = num;
            for (int p = 0; p < 5; p++) {
                for (const char *s = parts[p]; *s && off < cap - 2; s++)
                    buf[off++] = *s;
            }
            /* sector count as decimal */
            uint32_t secs = d->size;
            char secbuf[12]; int si = 11; secbuf[si] = '\0';
            if (secs == 0) { secbuf[--si] = '0'; }
            else { while (secs) { secbuf[--si] = (char)('0' + secs % 10); secs /= 10; } }
            for (const char *s = secbuf + si; *s && off < cap - 2; s++)
                buf[off++] = *s;
            const char *tail = " sectors\n";
            for (const char *s = tail; *s && off < cap - 2; s++)
                buf[off++] = *s;
        }
        buf[off] = '\0';
        regs->eax = off;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_PCI_INFO(239): render pci_devices[] as text.
     * EBX = buf, ECX = bufsz.  Returns bytes written.
     * ------------------------------------------------------------------ */
    case SYS_PCI_INFO: {
        char    *buf = (char *)(uintptr_t)regs->ebx;
        uint32_t cap = regs->ecx;
        if (!buf || cap == 0) { regs->eax = 0; break; }
        uint32_t off = 0;

#define PCI_APPEND(s) do { for (const char *_p = (s); *_p && off < cap - 2; _p++) buf[off++] = *_p; } while(0)
#define PCI_BYTE_HEX(v) do { \
    static const char _h[] = "0123456789ABCDEF"; \
    if (off + 2 < cap) { buf[off++] = _h[((v)>>4)&0xF]; buf[off++] = _h[(v)&0xF]; } } while(0)
#define PCI_WORD_HEX(v) do { PCI_BYTE_HEX((v)>>8); PCI_BYTE_HEX(v); } while(0)
#define PCI_DEC(v) do { \
    char _d[6]; int _i = 5; _d[_i] = '\0'; uint32_t _v = (v); \
    if (!_v) _d[--_i] = '0'; \
    else while (_v) { _d[--_i] = (char)('0' + _v % 10); _v /= 10; } \
    PCI_APPEND(_d + _i); } while(0)

        for (int i = 0; i < pci_device_count && off < cap - 96; i++) {
            const pci_device_t *d = &pci_devices[i];
            /* BB:DD.F  Class [CCSS]: Vendor Device (rev RR) [IRQ=N] */
            PCI_BYTE_HEX(d->bus);  buf[off++] = ':';
            PCI_BYTE_HEX(d->dev);  buf[off++] = '.';
            buf[off++] = (char)('0' + d->func);
            PCI_APPEND("  ");
            PCI_APPEND(pci_class_name(d->class_code, d->subclass));
            PCI_APPEND(" [");
            PCI_BYTE_HEX(d->class_code); PCI_BYTE_HEX(d->subclass);
            PCI_APPEND("]: ");
            /* Vendor name (fall back to hex) */
            const char *vname = pci_vendor_name(d->vendor_id);
            if (vname) { PCI_APPEND(vname); buf[off++] = ' '; }
            else { PCI_WORD_HEX(d->vendor_id); buf[off++] = ':'; }
            /* Device name (fall back to hex) */
            const char *dname = pci_device_name(d->vendor_id, d->device_id);
            if (dname) { PCI_APPEND(dname); }
            else { PCI_WORD_HEX(d->device_id); }
            PCI_APPEND("  (rev "); PCI_BYTE_HEX(d->revision_id); buf[off++] = ')';
            if (d->irq_line && d->irq_line != 0xFF) {
                PCI_APPEND("  IRQ="); PCI_DEC(d->irq_line);
            }
            buf[off++] = '\n';
        }
#undef PCI_APPEND
#undef PCI_BYTE_HEX
#undef PCI_WORD_HEX
#undef PCI_DEC
        buf[off] = '\0';
        regs->eax = off;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_NET_INFO(253): render active Ethernet netdev state as text.
     * EBX = buf, ECX = bufsz.  Returns bytes written.
     * ------------------------------------------------------------------ */
    case SYS_NET_INFO: {
        char    *buf = (char *)(uintptr_t)regs->ebx;
        uint32_t cap = regs->ecx;
        regs->eax = (uint32_t)net_lwip_info(buf, cap);
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_NET_CTL(254): control active Ethernet/lwIP state.
     * EBX = NET_CTL_* command. Returns 0 on success, -1 on error.
     * ------------------------------------------------------------------ */
    case SYS_NET_CTL: {
        regs->eax = (uint32_t)net_lwip_control((int)regs->ebx);
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_WGET(255): fetch an http:// URL and write the body to a VFS path.
     * EBX = url, ECX = outpath (user pointers; same direct-use convention as
     * SYS_OPEN).  Returns bytes saved (>=0), or negative: -1 fetch/parse
     * error, -2 write error, -(status) for a non-2xx HTTP status.
     * ------------------------------------------------------------------ */
    case SYS_WGET: {
        const char *url = (const char *)(uintptr_t)regs->ebx;
        const char *outpath = (const char *)(uintptr_t)regs->ecx;
        if (!url || !outpath) { regs->eax = (uint32_t)-1; break; }
        uint8_t *body = 0;
        uint32_t len = 0;
        int status = 0;
        int rc = wget_fetch(url, &body, &len, &status);
        if (rc != 0) { regs->eax = (uint32_t)-1; break; }
        if (status < 200 || status >= 300) {
            kfree(body);
            regs->eax = (uint32_t)(-status);
            break;
        }
        int wr = vfs_write_file(outpath, body, len);
        kfree(body);
        regs->eax = (wr == 0) ? len : (uint32_t)-2;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DELETE_FILE(208) / SYS_UNLINK(10): delete a VFS file.
     * EBX = path.  POSIX unlink() is aliased onto the same handler.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_DELETE_FILE:
    case SYS_UNLINK: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_delete_file(path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_RENAME_FILE(209) / SYS_RENAME(38): rename/move a file or directory.
     * EBX = old_path, ECX = new_path.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_RENAME_FILE:
    case SYS_RENAME: {
        const char *old_path = (const char *)(uintptr_t)regs->ebx;
        const char *new_path = (const char *)(uintptr_t)regs->ecx;
        if (!old_path || !new_path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_rename(old_path, new_path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DELETE_DIR(210) / SYS_RMDIR(40): delete an empty directory.
     * EBX = path.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_DELETE_DIR:
    case SYS_RMDIR: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_delete_dir(path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_MKDIR(39): create a directory.
     * EBX = path, ECX = mode (ignored -- no permission model).
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_MKDIR: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_mkdir(path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_KILL(37): deliver a signal to a target pid.
     * EBX = pid, ECX = signo.  Returns 0 on success, -1 on error
     * (no such pid, or invalid signo).
     *
     * No permission model yet -- any task can signal any other.  When
     * a user/kernel split lands, this will become the natural choke
     * point for credential checks.
     * ------------------------------------------------------------------ */
    case SYS_KILL: {
        int pid   = (int)regs->ebx;
        int signo = (int)regs->ecx;
        if (signo < 1 || signo > SIG_MAX) {
            regs->eax = (uint32_t)-1;
            break;
        }
        regs->eax = (sig_send_pid(pid, signo) == 0)
                    ? 0u : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_SIGNAL(48): install a handler for signo on the calling task.
     * EBX = signo, ECX = handler (SIG_DFL = 0, SIG_IGN = 1, or a
     * user-space function pointer).
     *
     * Returns the previous handler value, or (uint32_t)-1 on error
     * (out-of-range signo, or signo == SIGKILL/SIGSTOP).
     *
     * User-defined handlers are stored but not yet invoked (no ring-3
     * trampoline + sigreturn machinery yet).  SIG_DFL and SIG_IGN take
     * effect immediately because the in-kernel delivery path checks
     * those sentinels by pointer identity.
     * ------------------------------------------------------------------ */
    case SYS_SIGNAL: {
        int signo = (int)regs->ebx;
        sig_handler_t new_h = (sig_handler_t)(uintptr_t)regs->ecx;
        task_t *t = task_current();
        if (!t || signo < 1 || signo > SIG_MAX ||
            signo == SIGKILL || signo == SIGSTOP) {
            regs->eax = (uint32_t)-1;
            break;
        }
        sig_handler_t prev = sig_get_handler(t, signo);
        if (sig_set_handler(t, signo, new_h) != 0) {
            regs->eax = (uint32_t)-1;
            break;
        }
        regs->eax = (uint32_t)(uintptr_t)prev;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_FCNTL(55): minimal Linux-style fcntl.
     *   F_GETFL (3) -> returns the fd's flags
     *   F_SETFL (4) -> replaces them (only O_NONBLOCK is meaningful today)
     * Returns the flags / 0 on success, negative errno on bad fd / cmd.
     * ------------------------------------------------------------------ */
    case SYS_FCNTL: {
        int fd  = (int)regs->ebx;
        int cmd = (int)regs->ecx;
        long arg = (long)regs->edx;
        task_t *t = task_current();
        fd_entry_t *e = (t && t->fd_table) ? fd_get(t->fd_table, fd) : NULL;
        if (!e) { regs->eax = (uint32_t)-9; break; }   /* -EBADF */
        if (cmd == F_GETFL) {
            regs->eax = e->flags;
        } else if (cmd == F_SETFL) {
            /* Only the documented bits are honoured; everything else
             * silently dropped (Linux does similar masking). */
            e->flags = (uint32_t)(arg & FD_FLAG_NONBLOCK);
            regs->eax = 0u;
        } else {
            regs->eax = (uint32_t)-22;   /* -EINVAL */
        }
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_SIGRETURN(119): restore interrupted state from the sigframe
     * on the user stack.  Invoked indirectly by the trampoline embedded
     * in the sigframe -- userspace should never call this directly.
     *
     * On entry, regs->useresp points at the slot immediately after
     * ret_addr (i.e. signo's old slot), because the handler's `ret`
     * already popped ret_addr.  The sigframe starts at useresp - 4.
     *
     * On a corrupted / out-of-range sigframe we terminate the task
     * rather than try to limp on with whatever state we recover --
     * sigreturn from a missing or tampered frame is a recovery dead
     * end.
     * ------------------------------------------------------------------ */
    case SYS_SIGRETURN: {
        task_t *t = task_current();
        uint32_t base = regs->useresp - 4u;
        /* Range check: sigframe must lie inside the user stack range
         * we manage -- USER_STACK_PAGES pages below USER_STACK_TOP. */
        if (base < (USER_STACK_TOP - USER_STACK_PAGES * 4096u) ||
            base + sizeof(sigframe_t) > USER_STACK_TOP) {
            Serial_WriteString("[signal] sigreturn: out-of-range frame; "
                               "killing pid=");
            Serial_WriteDec(t ? (uint32_t)t->pid : 0u);
            Serial_WriteString("\n");
            if (t) t->state = TASK_DEAD;
            /* Fall through to task_yield -- we can't iret back to user. */
            task_yield();
            break;
        }
        sigframe_t sf;
        memcpy(&sf, (const void *)(uintptr_t)base, sizeof(sf));
        if (sf.magic != SIGFRAME_MAGIC) {
            Serial_WriteString("[signal] sigreturn: bad magic 0x");
            Serial_WriteHex(sf.magic);
            Serial_WriteString("; killing pid=");
            Serial_WriteDec(t ? (uint32_t)t->pid : 0u);
            Serial_WriteString("\n");
            if (t) t->state = TASK_DEAD;
            task_yield();
            break;
        }
        regs->eip      = sf.saved_eip;
        regs->cs       = sf.saved_cs;
        regs->eflags   = sf.saved_eflags;
        regs->useresp  = sf.saved_useresp;
        regs->ss       = sf.saved_ss;
        regs->eax      = sf.saved_eax;
        regs->ebx      = sf.saved_ebx;
        regs->ecx      = sf.saved_ecx;
        regs->edx      = sf.saved_edx;
        regs->esi      = sf.saved_esi;
        regs->edi      = sf.saved_edi;
        regs->ebp      = sf.saved_ebp;
        regs->ds       = sf.saved_ds;
        break;
    }

    /* ------------------------------------------------------------------
     * Admin syscalls (219..230).  Each calls task_is_admin() first; on
     * denial -1 is returned (currently unreachable — task_is_admin()
     * always returns true until a real user model lands).
     *
     * String args read directly from userspace (the calling task's PD
     * is live).  No copy_from_user yet — same convention as SYS_OPEN.
     * ------------------------------------------------------------------ */
    case SYS_REBOOT: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        admin_reboot();          /* noreturn on success */
        regs->eax = (uint32_t)-1;
        break;
    }
    case SYS_SHUTDOWN: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        admin_shutdown();        /* noreturn on success */
        regs->eax = (uint32_t)-1;
        break;
    }
    case SYS_SETMODE: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_setmode((const char *)regs->ebx);
        break;
    }
    case SYS_FGCOL: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_fgcol((const char *)regs->ebx);
        break;
    }
    case SYS_BGCOL: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_bgcol((const char *)regs->ebx);
        break;
    }
    case SYS_EJECT: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_eject();
        break;
    }
    case SYS_INSTALL: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_install();
        break;
    }
    /* Stepped headless installer for the GUI front-end (mxinstall.elf).
     * EBX = cmd (0 begin, 1 step, 2 finish, 3 list-drives); ECX = the matching
     * params / progress / drive-array pointer. */
    case SYS_INSTALL_EXEC: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        int   cmd = (int)regs->ebx;
        void *ptr = (void *)(uintptr_t)regs->ecx;
        /* NOTE: the install engine runs WITHOUT the FS big-lock held across the
         * step.  Holding vfs_fs_lock across a whole begin/step (the engine is
         * preemptible) stalled the copy, so we keep the v0.10.0 behaviour: the
         * engine's direct backend calls go unlocked.  The remaining race is the
         * installer's iso9660 read vs. another task's iso9660 read on the shared
         * sector scratch -- low severity (read/read, contained), to be closed in
         * the preemption phase by per-op locking of the engine's backend calls. */
        if      (cmd == 0) regs->eax = (uint32_t)install_exec_begin((const install_params_t *)ptr);
        else if (cmd == 1) regs->eax = (uint32_t)install_exec_step((install_progress_t *)ptr);
        else if (cmd == 2) regs->eax = (uint32_t)install_exec_finish((const install_params_t *)ptr);
        else if (cmd == 3) regs->eax = (uint32_t)install_exec_drives((install_drive_t *)ptr);
        else               regs->eax = (uint32_t)-1;
        break;
    }
    case SYS_MOUNT: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_mount((const char *)regs->ebx,
                                          (const char *)regs->ecx);
        break;
    }
    case SYS_UMOUNT: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_umount((const char *)regs->ebx);
        break;
    }
    case SYS_MKFS: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_mkfs((const char *)regs->ebx,
                                         (const char *)regs->ecx);
        break;
    }
    case SYS_SCHED_QUANTUM: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_sched_quantum((int)regs->ebx);
        break;
    }
    case SYS_VERBOSE: {
        if (!task_is_admin(NULL)) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (uint32_t)admin_verbose((int)regs->ebx);
        break;
    }
    case SYS_SHELL_READY: {
        /* Emit the `[shell:ready vt=N]` sync marker on COM1.  Gated on
         * g_serial_verbose so production boots don't pay the cost.
         * Userspace shell calls this before each prompt so the in-guest test
         * drivers' serial sync is identical to the kernel shell. */
        if (g_serial_verbose) {
            task_t *t = task_current();
            int vt = (t && t->tty >= 0) ? t->tty : 0;
            Serial_WriteString("[shell:ready vt=");
            char buf[12]; int n = 0; int v = vt;
            if (v == 0) buf[n++] = '0';
            while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n--) { Serial_WriteChar(buf[n]); }
            Serial_WriteString("]\n");
        }
        regs->eax = 0;
        break;
    }
    case SYS_CURSOR_POS: {
        uint32_t col = vesa_tty_is_ready() ? vesa_tty_get_col()
                                           : (uint32_t)t_column;
        uint32_t row = vesa_tty_is_ready() ? vesa_tty_get_row()
                                           : (uint32_t)t_row;
        regs->eax = ((col & 0xFFFFu) << 16) | (row & 0xFFFFu);
        break;
    }
    case SYS_VT_ENTER: {
        int slot = shell_enter_makmux_slot((int)regs->ebx != 0);
        regs->eax = (uint32_t)slot;
        break;
    }
    case SYS_VT_CLOSE: {
        regs->eax = (uint32_t)(int32_t)vtty_close_pid((int)regs->ebx);
        break;
    }
    case SYS_VT_OPEN_REQUEST: {
        regs->eax = (uint32_t)vtty_take_open_request();
        break;
    }
    case SYS_VT_STATE: {
        uint32_t mask = vtty_live_mask() & 0xFFFFu;
        /* High 16 bits = focused tty.  With makmux running that's the active
         * VT slot (0-3); with no VT children the root console (mak.sh0) is
         * focused, so report VTTY_ROOT_SLOT -- lets the userspace statusbar's
         * `command` widget find the root shell's foreground task. */
        uint32_t active = vtty_root_text_active()
                               ? (uint32_t)VTTY_ROOT_SLOT
                               : mask ? (uint32_t)(vtty_active() & 0xFFFF)
                               : (uint32_t)VTTY_ROOT_SLOT;
        regs->eax = (active << 16) | mask;
        break;
    }
    case SYS_VT_CLOCK_REQUEST: {
        regs->eax = (uint32_t)vtty_take_clock_toggle_request();
        break;
    }
    case SYS_GETHOSTNAME: {
        char *buf = (char *)regs->ebx;
        uint32_t size = regs->ecx;
        if (!buf || size == 0) { regs->eax = (uint32_t)-1; break; }
        /* Best-effort read of /etc/hostname; falls back to "makar".
         * No newline trimming for the read — but we strip the trailing
         * \n if the file ends with one (common case for hand-edited
         * /etc/hostname). */
        char hbuf[64];
        uint32_t got = 0;
        const char *src = "makar";
        uint32_t slen = 5;
        if (vfs_read_file("/etc/hostname", hbuf, sizeof(hbuf) - 1, &got) == 0 && got > 0) {
            hbuf[got] = '\0';
            while (got > 0 && (hbuf[got - 1] == '\n' || hbuf[got - 1] == '\r' ||
                               hbuf[got - 1] == ' '  || hbuf[got - 1] == '\t'))
                hbuf[--got] = '\0';
            if (got > 0) { src = hbuf; slen = got; }
        }
        uint32_t copy = (slen + 1 > size) ? (size - 1) : slen;
        for (uint32_t i = 0; i < copy; i++) buf[i] = src[i];
        buf[copy] = '\0';
        regs->eax = (uint32_t)copy;
        break;
    }

    case SYS_STATUSBAR: {
        /* Mechanism for userspace statusbar.elf: reserve/free the bottom row.
         * The renderer (which widgets, layout, .sbrc) lives in userspace. */
        int cmd = (int)regs->ebx;
        if (cmd == 0)      vesa_tty_set_status_visible(0);
        else if (cmd == 1) vesa_tty_set_status_visible(1);
        regs->eax = (uint32_t)vesa_tty_status_enabled();
        break;
    }

    case SYS_VT_OPEN_APP: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        regs->eax = (uint32_t)vtty_open_app(path);
        break;
    }

    case SYS_VT_TAKE_APP: {
        char *buf = (char *)regs->ebx;
        int   cap = (int)regs->ecx;
        if (!buf || cap <= 0) { regs->eax = 0; break; }
        regs->eax = (uint32_t)vtty_take_app_request(buf, cap);
        break;
    }

    case SYS_VT_SETNAME: {
        const char *name = (const char *)(uintptr_t)regs->ebx;
        task_t *me = task_current();
        if (me && me->tty >= 0)
            vtty_set_name(me->tty, name);
        regs->eax = 0;
        break;
    }

    case SYS_STATFS: {
        uint32_t *total = (uint32_t *)(uintptr_t)regs->ebx;
        uint32_t *freeb = (uint32_t *)(uintptr_t)regs->ecx;
        regs->eax = (uint32_t)vfs_statfs(total, freeb);
        break;
    }

    case SYS_VT_GETNAME: {
        int   slot = (int)regs->ebx;
        char *buf  = (char *)regs->ecx;
        int   cap  = (int)regs->edx;
        if (!buf || cap <= 0) { regs->eax = (uint32_t)-1; break; }
        const char *nm = vtty_get_name(slot);
        int i = 0;
        for (; nm[i] && i < cap - 1; i++) buf[i] = nm[i];
        buf[i] = '\0';
        regs->eax = (uint32_t)i;
        break;
    }

    case SYS_WHOAMI: {
        char *buf = (char *)regs->ebx;
        uint32_t size = regs->ecx;
        if (!buf || size == 0) { regs->eax = (uint32_t)-1; break; }
        const char *u = auth_current_user();
        if (!u) u = "user";
        uint32_t slen = 0;
        while (u[slen]) slen++;
        uint32_t copy = (slen + 1 > size) ? (size - 1) : slen;
        for (uint32_t i = 0; i < copy; i++) buf[i] = u[i];
        buf[copy] = '\0';
        regs->eax = (uint32_t)copy;
        break;
    }

    case SYS_IPC_SEND:
        regs->eax = (uint32_t)ipc_send((int)regs->ebx,
                                       (const ipc_msg_t *)(uintptr_t)regs->ecx);
        break;

    case SYS_IPC_RECV:
        regs->eax = (uint32_t)ipc_recv((int)regs->ebx,
                                       (ipc_msg_t *)(uintptr_t)regs->ecx);
        break;

    case SYS_IPC_SENDREC:
        regs->eax = (uint32_t)ipc_sendrec((int)regs->ebx,
                                          (ipc_msg_t *)(uintptr_t)regs->ecx);
        break;

    case SYS_IPC_NBRECV:
        regs->eax = (uint32_t)ipc_nbrecv((int)regs->ebx,
                                         (ipc_msg_t *)(uintptr_t)regs->ecx);
        break;

    /* ------------------------------------------------------------------
     * Shared pixel surfaces.  EBX/ECX carry the args; see kernel/surface.h.
     * SURFACE_MAP returns a userspace address (0 == failure, like mmap-ish
     * but NULL not MAP_FAILED); the others return the helper's int/uint.
     * ------------------------------------------------------------------ */
    case SYS_SURFACE_CREATE:
        regs->eax = (uint32_t)surface_create((int)regs->ebx, (int)regs->ecx);
        break;
    case SYS_SURFACE_MAP:
        regs->eax = surface_map((int)regs->ebx, task_current());
        break;
    case SYS_SURFACE_INFO:
        regs->eax = surface_info((int)regs->ebx);
        break;
    case SYS_SURFACE_DESTROY:
        regs->eax = (uint32_t)surface_destroy((int)regs->ebx, task_current());
        break;
    case SYS_SURFACE_UNMAP:
        regs->eax = (uint32_t)surface_unmap((int)regs->ebx, task_current());
        break;

    default:
        /* Unknown syscall - return -ENOSYS. */
        regs->eax = (uint32_t)-38;   /* -ENOSYS */
        break;
    }

    /* Slice 8 phase 4: deliver any user-handler signal that's pending
     * (and unmasked) before iret returns to ring 3.  No-op when the
     * frame is ring 0 or no handler is installed. */
    signal_check_user(regs);
}

/*
 * syscall_dispatch -- int 0x80 entry.  The gate clears IF; when preemptive
 * syscalls are enabled we re-enable interrupts for the duration of the call so
 * the timer can preempt a long syscall (the whole point of Phase C), then
 * disable again so the ISR epilogue's register-restore + iret runs atomically
 * (iret restores ring 3's IF=1).  Shared kernel state is protected by the
 * heap / PMM / task-pool / FS-disk / page-cache / net locks; noreturn paths
 * (task_exit, ring3_enter) manage IF themselves and never fall through here.
 */
void syscall_dispatch(registers_t *regs)
{
    /* Save the caller's IF and restore it on exit rather than forcing it.
     * Via int 0x80 the gate cleared IF, so this restores cli and the ISR
     * epilogue's restore+iret runs atomically (iret restores ring 3's IF=1).
     * ktest also calls this *directly* from a kernel task with IF=1; restoring
     * the caller's flags keeps the timer running for it (an unconditional cli
     * here froze a CLOCK_MONOTONIC busy-wait). */
    uint32_t fl;
    __asm__ volatile("pushfl; popl %0" : "=r"(fl) :: "memory");
    if (g_preempt_enabled) __asm__ volatile("sti");
    syscall_dispatch_inner(regs);
    __asm__ volatile("pushl %0; popfl" :: "r"(fl) : "memory", "cc");
}

void syscall_init(void)
{
    register_interrupt_handler(0x80, syscall_dispatch);
}
