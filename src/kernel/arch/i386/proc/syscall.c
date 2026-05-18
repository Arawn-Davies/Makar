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
#include <kernel/serial.h>
#include <kernel/vga.h>
#include <kernel/vesa_tty.h>
#include <kernel/ide.h>
#include <string.h>
#include <kernel/ktest.h>

/* USER_STACK_TOP - matches usertest.c / elf.c; defined locally to
 * avoid pulling those headers into syscall.c just for this one
 * constant.  Stack grows down from here in one mapped 4 KiB page. */
#define USER_STACK_TOP  0xBFFF0000u

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
        Serial_WriteString("[sys_exit] task pid=");
        Serial_WriteDec(t ? (uint32_t)t->pid : 0u);
        Serial_WriteString(" status=");
        Serial_WriteDec((uint32_t)regs->ebx);
        Serial_WriteString(" -> task_exit()\n");
        task_exit();   /* does not return */
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
            for (uint32_t i = 0; i < len; i++)
                Serial_WriteChar(buf[i]);
            regs->eax = len;
        } else if (e->kind == FD_KIND_SERIAL) {
            for (uint32_t i = 0; i < len; i++)
                Serial_WriteChar(buf[i]);
            regs->eax = len;
        } else {
            /* KEYBOARD and FILE: not writable through this fd today. */
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
        const char *path  = (const char *)(uintptr_t)regs->ebx;
        /* flags (ecx) reserved for future use */

        if (!path) { regs->eax = (uint32_t)-1; break; }

        task_t *cur = task_current();
        if (!cur || !cur->fd_table) { regs->eax = (uint32_t)-1; break; }

        int fd = fd_alloc(cur->fd_table);
        if (fd < 0) { regs->eax = (uint32_t)-1; break; }  /* too many open files */

        uint8_t *buf = (uint8_t *)kmalloc(SYSCALL_FILE_MAX);
        if (!buf)   { regs->eax = (uint32_t)-1; break; }

        uint32_t out_sz = 0;
        if (vfs_read_file(path, buf, SYSCALL_FILE_MAX, &out_sz) != 0) {
            kfree(buf);
            regs->eax = (uint32_t)-1;
            break;
        }

        fd_entry_t *e = &cur->fd_table->slots[fd];
        e->kind = FD_KIND_FILE;
        e->data = buf;
        e->size = out_sz;
        e->pos  = 0;

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
        if (!e || e->kind != FD_KIND_FILE) { regs->eax = (uint32_t)-1; break; }
        uint32_t new_pos;
        if (whence == 0)      new_pos = (uint32_t)offset;
        else if (whence == 1) new_pos = (uint32_t)((int)e->pos + offset);
        else if (whence == 2) new_pos = (uint32_t)((int)e->size + offset);
        else { regs->eax = (uint32_t)-1; break; }
        if (new_pos > e->size) new_pos = e->size;
        e->pos = new_pos;
        regs->eax = new_pos;
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
        /* SYS_PUTCH_AT cells carry their own colour attribute, so writing
         * each cell mutates the default pane's fg/bg.  Save the pane
         * colours up-front and restore at the end so apps that paint
         * coloured chrome (kbtester, future status bars) don't leave the
         * shell stuck in their palette after exit. */
        vesa_pane_t *dp = vesa_tty_is_ready() ? vesa_tty_default_pane() : NULL;
        uint32_t saved_fg = dp ? dp->fg : 0;
        uint32_t saved_bg = dp ? dp->bg : 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t col = cells[i].col;
            uint8_t row = cells[i].row;
            uint8_t ch  = cells[i].ch;
            uint8_t clr = cells[i].clr;
            t_putentryat((char)ch, clr, col, row);
            if (dp) {
                vesa_tty_setcolor(s_vga_palette[clr & 0x0F],
                                  s_vga_palette[(clr >> 4) & 0x0F]);
                vesa_tty_put_at((char)ch, col, row);
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
    case SYS_SET_CURSOR:
        t_set_cursor((size_t)regs->ebx, (size_t)regs->ecx);
        break;

    /* ------------------------------------------------------------------
     * SYS_TTY_CLEAR(203): fill screen with spaces.
     * EBX = VGA colour attribute (e.g. 0x07 = white-on-black).
     * ------------------------------------------------------------------ */
    case SYS_TTY_CLEAR:
        t_fill((uint8_t)regs->ebx);
        break;

    /* ------------------------------------------------------------------
     * SYS_TERM_SIZE(204): query terminal dimensions.
     * Returns EAX = (cols << 16) | rows.
     * ------------------------------------------------------------------ */
    case SYS_TERM_SIZE: {
        uint32_t cols, rows;
        if (vesa_tty_is_ready()) {
            cols = vesa_tty_get_cols();
            rows = vesa_tty_get_rows();
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
     * SYS_DELETE_FILE(208): delete a VFS file.
     * EBX = path.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_DELETE_FILE: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_delete_file(path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_RENAME_FILE(209): rename/move a file or directory.
     * EBX = old_path, ECX = new_path.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_RENAME_FILE: {
        const char *old_path = (const char *)(uintptr_t)regs->ebx;
        const char *new_path = (const char *)(uintptr_t)regs->ecx;
        if (!old_path || !new_path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_rename(old_path, new_path) == 0) ? 0 : (uint32_t)-1;
        break;
    }

    /* ------------------------------------------------------------------
     * SYS_DELETE_DIR(210): delete an empty directory.
     * EBX = path.
     * Returns 0 on success, (uint32_t)-1 on error.
     * ------------------------------------------------------------------ */
    case SYS_DELETE_DIR: {
        const char *path = (const char *)(uintptr_t)regs->ebx;
        if (!path) { regs->eax = (uint32_t)-1; break; }
        regs->eax = (vfs_delete_dir(path) == 0) ? 0 : (uint32_t)-1;
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
        /* Range check: sigframe must lie inside the user stack page
         * range we manage.  USER_STACK_TOP is the top of the one
         * mapped 4 KiB stack page (stack grows down from there). */
        if (base < (USER_STACK_TOP - 4096u) ||
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
