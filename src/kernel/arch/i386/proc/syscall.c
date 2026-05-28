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
#include <kernel/fd.h>
#include <kernel/signal.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/elf.h>
#include <kernel/serial.h>
#include <kernel/vga.h>
#include <kernel/vesa_tty.h>
#include <kernel/vesa.h>
#include <kernel/vtty.h>
#include <kernel/vt.h>
#include <kernel/ide.h>
#include <kernel/timer.h>
#include <kernel/rtc.h>
#include <string.h>
#include <kernel/ktest.h>
#include <kernel/admin.h>

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

/* -------------------------------------------------------------------------
 * Checkpoint tracking
 * ------------------------------------------------------------------------- */

volatile uint32_t g_ring3_last_cp = 0;

/* -------------------------------------------------------------------------
 * syscall_dispatch
 * ------------------------------------------------------------------------- */

void syscall_dispatch(registers_t *regs)
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
                    c->state = TASK_DEAD;
                    /* Counterpart to SYS_EXECVE's "child takes focus" rule:
                     * when the reaper sees the foreground child go zombie,
                     * hand focus back to the wait4-ing parent so its REPL
                     * (sh.elf, kernel shell, ...) becomes the next reader. */
                    keyboard_set_focus(me);
                    vtty_set_foreground(me->tty, me);
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

        enum { EXECVE_MAX_ARGC = 16, EXECVE_ARG_MAX = 256 };
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
            keyboard_set_focus(me);
            vtty_set_foreground(me->tty, me);
        }

        /* elf_exec swaps the PD and iret's to the new entry on success
         * (never returns).  Any return value here means it failed; pass
         * the negative errno back to the caller via EAX. */
        int rc = elf_exec(s_path, kargc, (const char *const *)s_argv);
        regs->eax = (uint32_t)(int32_t)rc;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_FORK(2): COW-clone the calling task.
     * Returns child pid in parent, 0 in child, -EAGAIN on failure.
     * Child returns through fork_child_iret, never through this dispatch.
     * ------------------------------------------------------------------ */
    case SYS_FORK: {
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
            /* Line-buffered stdin with echo, backspace, and cursor editing. */
            static char s_stdin_line[256];
            uint32_t cap = (len < sizeof(s_stdin_line)) ? len : (uint32_t)sizeof(s_stdin_line);
            shell_readline(s_stdin_line, (size_t)cap);
            uint32_t n = (uint32_t)strlen(s_stdin_line);
            /* Append '\n' so callers see a complete line (like a real terminal). */
            if (n < cap - 1) { s_stdin_line[n++] = '\n'; s_stdin_line[n] = '\0'; }
            if (n > len) n = len;
            memcpy(buf, s_stdin_line, n);
            regs->eax = n;
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
    case SYS_KEYBOARD_RAW:
        keyboard_set_raw((int)regs->ebx);
        regs->eax = 0;
        break;

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
     * SYS_UPTIME(214): return the kernel tick counter (100 Hz).
     * Apps that need wall-clock duration (kbtester's hold-Esc, future
     * `clock` widget) can compute (uptime - t0) instead of counting
     * input events whose rate depends on PS/2 typematic settings.
     * ------------------------------------------------------------------ */
    case SYS_UPTIME:
        regs->eax = timer_get_ticks();
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
        tv->tv_usec = (int32_t)((timer_get_ticks() % 100u) * 10000u);
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
            ts->tv_nsec = (int32_t)((timer_get_ticks() % 100u) * 10000000u);
        } else if (clk == CLOCK_MONOTONIC) {
            uint32_t ticks = timer_get_ticks();
            ts->tv_sec  = (int32_t)(ticks / 100u);
            ts->tv_nsec = (int32_t)((ticks % 100u) * 10000000u);
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

        /* Drawable area excludes the status row.  vesa_tty_get_rows()
         * is in cell units; cell_h derives from the FB / row count so
         * we honour whatever font scale the operator selected. */
        uint32_t rows = vesa_tty_get_rows();
        uint32_t cell_h = rows ? (fb->height / rows) : 0;
        int32_t y_max = (int32_t)fb->height;
        if (cell_h && y_max > (int32_t)cell_h
            && VESA_TTY_STATUS_ROWS > 0)
            y_max -= (int32_t)(cell_h * VESA_TTY_STATUS_ROWS);
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
        vesa_tty_set_caret_style(regs->ebx);
        regs->eax = prev;
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
            /* Read existing file content into the buffer.  Size the
             * allocation to the actual file (probed via vfs_stat), not
             * SYSCALL_FILE_MAX -- otherwise every open()/close() pair on
             * a tiny log file would kmalloc + kfree 8 MiB, which is both
             * slow on TCG and stressful for the kernel heap.  For writable
             * fds, round up to at least SYSCALL_FILE_INITIAL so the first
             * write doesn't have to grow immediately. */
            vfs_stat_info_t si;
            uint32_t cap;
            if (vfs_stat(path, &si) == 0) {
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

        /* Align the current break up to the next page boundary, then map
         * all pages needed to reach new_brk. */
        uint32_t cur_page = (t->user_brk + 0xFFFu) & ~0xFFFu;
        uint32_t new_page = (new_brk     + 0xFFFu) & ~0xFFFu;

        for (uint32_t va = cur_page; va < new_page; va += 0x1000u) {
            uint32_t phys = pmm_alloc_frame();
            if (phys == PMM_ALLOC_ERROR) {
                /* Return what we managed to allocate so far. */
                regs->eax = t->user_brk;
                goto brk_done;
            }
            memset((void *)phys, 0, 0x1000u);
            vmm_map_page(t->page_dir, va, phys,
                         VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        }
        t->user_brk = new_brk;
        regs->eax   = new_brk;
    brk_done:
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
        struct rd_ctx { uint32_t target; uint32_t cur; int found;
                        const char *name; int is_dir; };
        struct rd_ctx ctx = { idx, 0, 0, 0, 0 };
        /* Callback captures the Nth entry into a static buffer.  The
         * complete() backends can be re-entered (slow), but a static
         * buffer is fine in non-reentrant kernel context. */
        static char  s_name[DIRENT_NAME_MAX];
        static int   s_is_dir;
        static struct rd_ctx *s_ctx;
        s_ctx = &ctx;
        void cb(const char *n, int is_dir, void *vctx) {
            struct rd_ctx *c = (struct rd_ctx *)vctx;
            if (c->found) return;
            if (c->cur == c->target) {
                uint32_t i = 0;
                while (n[i] && i < DIRENT_NAME_MAX - 1) { s_name[i] = n[i]; i++; }
                s_name[i] = '\0';
                s_is_dir = is_dir;
                c->found = 1;
            }
            c->cur++;
        }
        if (vfs_complete(path, "", cb, &ctx) != 0) {
            regs->eax = (uint32_t)-1; break;
        }
        if (!ctx.found) { regs->eax = 0; break; }
        memset(ude, 0, sizeof(*ude));
        uint32_t h = 2166136261u;
        for (const char *q = s_name; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
        ude->d_ino  = h;
        ude->d_type = s_is_dir ? DT_DIR : DT_REG;
        uint32_t i = 0;
        while (s_name[i] && i < DIRENT_NAME_MAX - 1) { ude->d_name[i] = s_name[i]; i++; }
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
    case SYS_GETKEY:
        regs->eax = (uint32_t)(uint8_t)keyboard_getchar();
        break;

    /* ------------------------------------------------------------------
     * SYS_PUTCH_AT(201): write an array of screen cells.
     * EBX = pointer to tty_cell_t[], ECX = count.
     * ------------------------------------------------------------------ */
    case SYS_PUTCH_AT: {
        const tty_cell_t *cells = (const tty_cell_t *)(uintptr_t)regs->ebx;
        uint32_t n = regs->ecx;
        if (!cells || n == 0) { regs->eax = 0; break; }
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
        vesa_pane_t *dp = (focused && vesa_tty_is_ready())
                              ? vesa_tty_default_pane() : NULL;
        uint32_t saved_fg = dp ? dp->fg : 0;
        uint32_t saved_bg = dp ? dp->bg : 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t col = cells[i].col;
            uint8_t row = cells[i].row;
            uint8_t ch  = cells[i].ch;
            uint8_t clr = cells[i].clr;
            uint32_t fg = s_vga_palette[clr & 0x0F];
            uint32_t bg = s_vga_palette[(clr >> 4) & 0x0F];

            if (vt) {
                vt_set_color(vt, fg, bg);
                vt_put_at(vt, (char)ch, col, row);
            }
            if (focused) {
                t_putentryat((char)ch, clr, col, row);
                if (dp) {
                    vesa_tty_setcolor(fg, bg);
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
        /* Report the *drawable* area, not the full framebuffer.  The
         * bottom VESA_TTY_STATUS_ROWS row is reserved for the tmux-style
         * VT bar -- fullscreen apps (maktop, clock, vix) that paint up
         * to (rows-1) would otherwise stomp the bar.  Resolution-aware
         * because both vesa_tty_get_rows() and the constant scale with
         * the chosen mode. */
        uint32_t cols, rows;
        if (vesa_tty_is_ready()) {
            cols = vesa_tty_get_cols();
            rows = vesa_tty_get_rows();
            if (rows > VESA_TTY_STATUS_ROWS) rows -= VESA_TTY_STATUS_ROWS;
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
         * Userspace shell calls this before each prompt so ui_test.sh's
         * wait_for_serial behaviour is identical to the kernel shell. */
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
        uint32_t active = (uint32_t)(vtty_active() & 0xFFFF);
        uint32_t mask = vtty_live_mask() & 0xFFFFu;
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

void syscall_init(void)
{
    register_interrupt_handler(0x80, syscall_dispatch);
}
