#ifndef _USERSPACE_SYSCALL_H
#define _USERSPACE_SYSCALL_H

/* Syscall numbers — Linux i386 ABI subset + Makar extensions. */
#define SYS_EXIT       1
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_LSEEK      19
#define SYS_BRK        45
#define SYS_DEBUG      100
#define SYS_YIELD      158
#define SYS_GETKEY     200
#define SYS_PUTCH_AT   201
#define SYS_SET_CURSOR 202
#define SYS_TTY_CLEAR  203
#define SYS_TERM_SIZE  204
#define SYS_WRITE_FILE 205
#define SYS_LS_DIR     206
#define SYS_DISK_INFO    207
#define SYS_DELETE_FILE  208
#define SYS_RENAME_FILE  209
#define SYS_DELETE_DIR   210
#define SYS_WRITE_SERIAL 211
#define SYS_KEYBOARD_RAW 212
#define SYS_SHELL_CLEAR  213

/* open() flags */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2

/* lseek() whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* VGA colour attribute helpers */
#define VGA_CLR(fg, bg)  ((unsigned char)((fg) | ((bg) << 4)))
#define VGA_BLACK        0
#define VGA_BLUE         1
#define VGA_GREEN        2
#define VGA_CYAN         3
#define VGA_RED          4
#define VGA_MAGENTA      5
#define VGA_BROWN        6
#define VGA_LGREY        7
#define VGA_DGREY        8
#define VGA_LBLUE        9
#define VGA_LGREEN       10
#define VGA_LCYAN        11
#define VGA_LRED         12
#define VGA_LMAGENTA     13
#define VGA_YELLOW       14
#define VGA_WHITE        15

/* One screen cell passed to sys_putch_at(). */
typedef struct { unsigned char col, row, ch, clr; } tty_cell_t;

/* Key sentinels returned by sys_getkey() (unsigned byte values).  Mirrors
 * <kernel/keyboard.h> KEY_* — kept in sync by hand because the userspace
 * build doesn't see kernel headers. */
#define KEY_ARROW_UP    0x80
#define KEY_ARROW_DOWN  0x81
#define KEY_ARROW_LEFT  0x82
#define KEY_ARROW_RIGHT 0x83
#define KEY_F1          0x84
#define KEY_F2          0x85
#define KEY_F3          0x86
#define KEY_F4          0x87
#define KEY_FOCUS_GAIN  0x88
#define KEY_F5          0x89
#define KEY_F6          0x8A
#define KEY_F7          0x8B
#define KEY_F8          0x8C
#define KEY_F9          0x8D
#define KEY_F10         0x8E
#define KEY_F11         0x8F
#define KEY_F12         0x90
#define KEY_SHIFT_DOWN  0x91
#define KEY_CTRL_DOWN   0x92
#define KEY_ALT_DOWN    0x93
#define KEY_CAPS_TOGGLE 0x94
#define KEY_SUPER_DOWN  0x95
#define KEY_MENU_DOWN   0x96
#define KEY_CTRL_S      0x13
#define KEY_CTRL_Q      0x11
#define KEY_CTRL_C      0x03

/* Raw syscall stubs. */
static inline long syscall1(long nr, long a1)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"(nr), "b"(a1) : "memory");
    return ret;
}

static inline long syscall2(long nr, long a1, long a2)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"(nr), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline long syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"(nr), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

/* POSIX-compatible wrappers. */

static inline void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static inline long sys_read(int fd, void *buf, unsigned int len)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)len);
}

static inline long sys_write(int fd, const void *buf, unsigned int len)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)len);
}

/* Write to COM1 serial only (does not touch the framebuffer). Useful for
 * silent diagnostics. For output the user should also see, prefer
 * sys_write(2, ...) which writes to both the screen and serial. */
static inline long sys_write_serial(const void *buf, unsigned int len)
{
    return syscall2(SYS_WRITE_SERIAL, (long)buf, (long)len);
}

/* Enable (on=1) or disable (on=0) raw keyboard delivery for the calling
 * app — see <kernel/keyboard.h> keyboard_set_raw().  Pair every enable
 * with a disable on the way out; the kernel forces raw=0 after exec
 * returns as a safety net, but apps that want to keep cooked-mode
 * shortcuts working for the operator should still clean up themselves. */
static inline void sys_keyboard_raw(int on)
{
    syscall1(SYS_KEYBOARD_RAW, (long)on);
}

/* Reset the terminal to the shell's default palette and clear it — the
 * same code path the `clear` shell command executes.  Use this from
 * apps that paint custom chrome and want to leave the screen in a known
 * state on exit; plain sys_tty_clear doesn't restore the pane palette. */
static inline void sys_shell_clear(void)
{
    syscall1(SYS_SHELL_CLEAR, 0);
}

static inline int sys_open(const char *path, int flags)
{
    return (int)syscall2(SYS_OPEN, (long)path, (long)flags);
}

static inline int sys_close(int fd)
{
    return (int)syscall1(SYS_CLOSE, (long)fd);
}

static inline long sys_lseek(int fd, int offset, int whence)
{
    return syscall3(SYS_LSEEK, (long)fd, (long)offset, (long)whence);
}

static inline long sys_brk(void *addr)
{
    return syscall1(SYS_BRK, (long)addr);
}

static inline void sys_yield(void)
{
    syscall1(SYS_YIELD, 0);
}

/* Raw single-char keyboard read; returns unsigned byte (0x80-0x83 = arrows). */
static inline int sys_getkey(void)
{
    return (int)syscall1(SYS_GETKEY, 0);
}

/* Write n screen cells at their specified positions. */
static inline int sys_putch_at(const tty_cell_t *cells, unsigned int n)
{
    return (int)syscall2(SYS_PUTCH_AT, (long)cells, (long)n);
}

/* Move the hardware cursor. */
static inline void sys_set_cursor(unsigned int col, unsigned int row)
{
    syscall2(SYS_SET_CURSOR, (long)col, (long)row);
}

/* Clear screen filling with VGA colour attribute clr. */
static inline void sys_tty_clear(unsigned char clr)
{
    syscall1(SYS_TTY_CLEAR, (long)clr);
}

/* Return terminal size as (cols << 16) | rows. */
static inline long sys_term_size(void)
{
    return syscall1(SYS_TERM_SIZE, 0);
}

static inline unsigned int sys_term_cols(void)
{
    return (unsigned int)(sys_term_size() >> 16) & 0xFFFF;
}

static inline unsigned int sys_term_rows(void)
{
    return (unsigned int)(sys_term_size() & 0xFFFF);
}

/* Create or overwrite a VFS file. Returns 0 on success, -1 on error. */
static inline int sys_write_file(const char *path, const void *buf, unsigned int len)
{
    return (int)syscall3(SYS_WRITE_FILE, (long)path, (long)buf, (long)len);
}

/* List a VFS directory into buf. Returns bytes written. */
static inline int sys_ls_dir(const char *path, char *buf, unsigned int bufsz)
{
    return (int)syscall3(SYS_LS_DIR, (long)path, (long)buf, (long)bufsz);
}

/* Get disk drive info as text. Returns bytes written. */
static inline int sys_disk_info(char *buf, unsigned int bufsz)
{
    return (int)syscall2(SYS_DISK_INFO, (long)buf, (long)bufsz);
}

/* Delete a file. Returns 0 on success, -1 on error. */
static inline int sys_delete_file(const char *path)
{
    return (int)syscall1(SYS_DELETE_FILE, (long)path);
}

/* Rename or move a file or directory. Returns 0 on success, -1 on error. */
static inline int sys_rename_file(const char *old_path, const char *new_path)
{
    return (int)syscall2(SYS_RENAME_FILE, (long)old_path, (long)new_path);
}

/* Delete an empty directory. Returns 0 on success, -1 on error. */
static inline int sys_delete_dir(const char *path)
{
    return (int)syscall1(SYS_DELETE_DIR, (long)path);
}

#endif
