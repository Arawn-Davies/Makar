#ifndef _USERSPACE_SYSCALL_H
#define _USERSPACE_SYSCALL_H

/* Syscall numbers - Linux i386 ABI subset + Makar extensions. */
#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_READ       3
#define SYS_EXECVE     11
#define SYS_CHDIR      12
#define SYS_WAIT4      114
/* wait4 `options` flags */
#define WNOHANG        1
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_LSEEK      19
#define SYS_GETPID     20
#define SYS_DUP        41
#define SYS_PIPE       42
#define SYS_DUP2       63
#define SYS_KILL       37
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_UNLINK     10
#define SYS_BRK        45
#define SYS_MUNMAP     91
#define SYS_MMAP2     192
#define SYS_SIGNAL     48
#define SYS_GETPPID    64
#define SYS_GETTIMEOFDAY 78
#define SYS_CLOCK_GETTIME 265
#define CLOCK_REALTIME    0
#define CLOCK_MONOTONIC   1

/* access(2) mode bits.  No permission model -- all four behave as F_OK. */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* Linux i386 layouts -- must match kernel/syscall.h.  Each struct has
 * its own guard macro so the userspace + kernel headers can coexist
 * when in-OS TCC rebuilds the kernel: kernel timer.h pulls in stdio.h
 * which pulls in this header, while kernel source also independently
 * includes kernel/syscall.h.  Whichever header is included first wins
 * the struct definition; the second skips. */
#ifndef _MAKAR_STRUCT_TIMEVAL_DEFINED
#define _MAKAR_STRUCT_TIMEVAL_DEFINED
struct timeval  { int tv_sec; int tv_usec; };
#endif
#ifndef _MAKAR_STRUCT_TIMESPEC_DEFINED
#define _MAKAR_STRUCT_TIMESPEC_DEFINED
struct timespec { int tv_sec; int tv_nsec; };
#endif
#define SYS_SIGRETURN  119
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
#define SYS_PCI_INFO     239
#define SYS_DELETE_FILE  208
#define SYS_RENAME_FILE  209
#define SYS_DELETE_DIR   210
#define SYS_WRITE_SERIAL 211
#define SYS_KEYBOARD_RAW 212
#define SYS_SHELL_CLEAR  213
#define SYS_UPTIME       214
#define SYS_GETCWD       215
#define SYS_FB_INFO      216
#define SYS_DRAW_LINE    217
#define SYS_CARET_STYLE  218
/* Admin syscalls (privileged operations).  task_is_admin() gates each;
 * currently always-true (no user model).  Negative return = denied or
 * failed; helper-specific error code conventions in kernel/admin.h. */
#define SYS_REBOOT        219
#define SYS_SHUTDOWN      220
#define SYS_SETMODE       221
#define SYS_FGCOL         222
#define SYS_BGCOL         223
#define SYS_EJECT         224
#define SYS_INSTALL       238
#define SYS_MOUNT         225
#define SYS_UMOUNT        226
#define SYS_MKFS          227
#define SYS_SCHED_QUANTUM 228
#define SYS_VERBOSE       229
#define SYS_GETHOSTNAME   230
#define SYS_WHOAMI        247   /* moved off 240 (Linux futex) */
#define SYS_STATUSBAR     241
#define SYS_VT_OPEN_APP   242
#define SYS_VT_TAKE_APP   248   /* moved off 243 (Linux set_thread_area) */
#define SYS_VT_SETNAME    244
#define SYS_VT_GETNAME    245
#define SYS_STATFS        246
#define SYS_SHELL_READY   231
#define SYS_CURSOR_POS    232
#define SYS_VT_ENTER      233
#define SYS_VT_CLOSE      234
#define SYS_VT_OPEN_REQUEST 235
#define SYS_VT_STATE      236
#define SYS_VT_CLOCK_REQUEST 237
#define SYS_FCNTL        55
#define SYS_STAT        106
#define SYS_FSTAT       108
#define SYS_READDIR     141
#define SYS_NET_INFO    253

/* dirent shape -- must match kernel struct dirent in kernel/syscall.h. */
#define DIRENT_NAME_MAX 256
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#ifndef _MAKAR_STRUCT_DIRENT_DEFINED
#define _MAKAR_STRUCT_DIRENT_DEFINED
struct dirent {
    unsigned int   d_ino;
    unsigned char  d_type;
    unsigned char  __pad[3];
    char           d_name[DIRENT_NAME_MAX];
};
#endif

/* fcntl cmds */
#define F_GETFL          3
#define F_SETFL          4

/* O_NONBLOCK for F_SETFL on stdin (fd 0).  Matches Linux i386 0x800. */
#define O_NONBLOCK       0x800

/* read(2) errno-style return for "no data and non-blocking". */
#define EAGAIN           11

/* Signal numbers (Linux i386 ABI subset).  Mirrors <kernel/signal.h>;
 * kept in sync by hand since the userspace build doesn't see kernel
 * headers. */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

/* sig_handler_t function pointer + SIG_DFL/SIG_IGN sentinels (POSIX). */
typedef void (*sig_handler_t)(int);
#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* open() flags -- low 2 bits are access mode; the rest are status flags.
 * Values mirror Linux i386. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0100
#define O_TRUNC     01000
#define O_APPEND    02000

/* Linux i386 struct stat (must match kernel/syscall.h). */
#ifndef _MAKAR_STRUCT_STAT_DEFINED
#define _MAKAR_STRUCT_STAT_DEFINED
struct stat {
    unsigned int   st_dev;
    unsigned int   st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned int   st_rdev;
    unsigned int   st_size;
    unsigned int   st_blksize;
    unsigned int   st_blocks;
    unsigned int   st_atime;
    unsigned int   st_atime_nsec;
    unsigned int   st_mtime;
    unsigned int   st_mtime_nsec;
    unsigned int   st_ctime;
    unsigned int   st_ctime_nsec;
    unsigned int   __unused4;
    unsigned int   __unused5;
};
#endif
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

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
 * <kernel/keyboard.h> KEY_* - kept in sync by hand because the userspace
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
#define KEY_PAGE_UP     0x97
#define KEY_PAGE_DOWN   0x98
#define KEY_CTRL_S      0x13
#define KEY_CTRL_Q      0x11
#define KEY_CTRL_C      0x03

/* Raw syscall stubs. */
static inline long syscall0(long nr)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"(nr) : "memory");
    return ret;
}

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

static inline long syscall4(long nr, long a1, long a2, long a3, long a4)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"(nr), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

/* mmap(2) prot/flags (Linux i386).  Only anonymous mappings are supported. */
#define PROT_NONE      0
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     ((void *)-1)

static inline void *sys_mmap(void *addr, unsigned long len, int prot,
                             int flags, int fd, long off)
{
    (void)fd; (void)off;   /* anonymous only -- fd/off ignored */
    return (void *)syscall4(SYS_MMAP2, (long)addr, (long)len, (long)prot, (long)flags);
}

static inline int sys_munmap(void *addr, unsigned long len)
{
    return (int)syscall2(SYS_MUNMAP, (long)addr, (long)len);
}

/* POSIX-compatible wrappers. */

static inline void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    /* SYS_EXIT never returns; the for(;;) is dead code that just
     * satisfies the compiler's "noreturn function returned" check.
     * Previously used __builtin_unreachable() but TCC v0.9.27 doesn't
     * implement that builtin (breaks in-OS sh.elf rebuild). */
    for (;;) { }
}

/* fork(2): clone the calling process via COW.  Returns child pid in the
 * parent, 0 in the child, or a negative errno on failure (typically
 * -EAGAIN if the task pool is full or PMM is exhausted). */
static inline int sys_fork(void)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "0"((long)SYS_FORK) : "memory");
    return (int)ret;
}

/* wait4(2): block until a child task becomes a zombie; write its
 * exit status into *status (if non-NULL) and return the child's pid.
 *
 *   pid > 0   -- wait for exactly that child
 *   pid == -1 -- wait for any child
 *   options   -- WNOHANG returns 0 immediately if no zombie is ready
 *
 * Returns child pid on success, 0 if WNOHANG and no zombie, or
 * negative errno (-ECHILD if the caller has no children).  rusage is
 * not implemented (always pass NULL / ignored). */
static inline int sys_wait4(int pid, int *status, int options)
{
    return (int)syscall3(SYS_WAIT4, (long)pid, (long)status, (long)options);
}

/* wait(2): POSIX shorthand for wait4(-1, status, 0). */
static inline int sys_wait(int *status)
{
    return sys_wait4(-1, status, 0);
}

/* execve(2): replace the calling task's address space with the ELF at
 * `path`.  argv is a NULL-terminated array of pointers to argument
 * strings (POSIX convention; argv[0] is the program name).  envp is
 * currently ignored by the kernel -- pass NULL.
 *
 * On success this call does NOT return: control resumes at the new
 * ELF's entry point with argc/argv/envp on the user stack.  On failure
 * (file not found, malformed ELF, OOM) returns negative errno and the
 * original address space is still active. */
static inline int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
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
 * app - see <kernel/keyboard.h> keyboard_set_raw().  Pair every enable
 * with a disable on the way out; the kernel forces raw=0 after exec
 * returns as a safety net, but apps that want to keep cooked-mode
 * shortcuts working for the operator should still clean up themselves. */
static inline void sys_keyboard_raw(int on)
{
    syscall1(SYS_KEYBOARD_RAW, (long)on);
}

/* Reset the terminal to the shell's default palette and clear it - the
 * same code path the `clear` shell command executes.  Use this from
 * apps that paint custom chrome and want to leave the screen in a known
 * state on exit; plain sys_tty_clear doesn't restore the pane palette. */
static inline void sys_shell_clear(void)
{
    syscall1(SYS_SHELL_CLEAR, 0);
}

/* Kernel tick counter (100 Hz).  Use for wall-clock duration measurement
 * - counting input events alone is unreliable because the rate depends
 * on the PS/2 typematic configuration. */
static inline unsigned int sys_uptime(void)
{
    return (unsigned int)syscall1(SYS_UPTIME, 0);
}

/* Copy the calling task's cwd into `buf` (NUL-terminated).
 * Returns strlen on success, -1 on error (buf NULL, size 0, or too small). */
static inline int sys_getcwd(char *buf, unsigned int size)
{
    return (int)syscall2(SYS_GETCWD, (long)buf, (long)size);
}

/* getpid(2) / getppid(2): identity accessors.  pid 1 = idle task.
 * parent_pid 0 means the caller was created directly by the kernel
 * (no userspace ancestor). */
static inline int sys_getpid(void)  { return (int)syscall1(SYS_GETPID,  0); }
static inline int sys_getppid(void) { return (int)syscall1(SYS_GETPPID, 0); }

/* gettimeofday(2) / clock_gettime(2).  REALTIME is RTC-derived seconds
 * since 1970-01-01 UTC; tv_usec/tv_nsec resolution is 10 ms (PIT 100 Hz
 * tick modulo), NOT real microseconds.  MONOTONIC counts seconds since
 * boot and never goes backwards. */
static inline int sys_gettimeofday(struct timeval *tv)
{
    return (int)syscall2(SYS_GETTIMEOFDAY, (long)tv, 0);
}
static inline int sys_clock_gettime(int clk, struct timespec *ts)
{
    return (int)syscall2(SYS_CLOCK_GETTIME, (long)clk, (long)ts);
}

/* chdir(2): change the calling task's cwd to `path`.  Delegates to the
 * kernel's vfs_cd which normalises and validates against the live VFS.
 * Returns 0 on success, -1 if the path is missing or not a directory. */
static inline int sys_chdir(const char *path)
{
    return (int)syscall1(SYS_CHDIR, (long)path);
}

/* Query pixel-mode framebuffer geometry.  Returns (width << 16) | height
 * when VESA is up, 0 when VGA-only (graphical apps fall back to cell mode). */
static inline unsigned int sys_fb_info(void)
{
    return (unsigned int)syscall1(SYS_FB_INFO, 0);
}

static inline unsigned int sys_fb_width(void)  { return (sys_fb_info() >> 16) & 0xFFFFu; }
static inline unsigned int sys_fb_height(void) { return  sys_fb_info()        & 0xFFFFu; }

/* Bresenham line in framebuffer pixels.  Clipped to the drawable area
 * (status row excluded).  Returns 0 on success, -1 when no FB. */
static inline int sys_draw_line(int x0, int y0, int x1, int y1, unsigned int rgb)
{
    unsigned int xy0 = ((unsigned int)(x0 & 0xFFFF) << 16) | (unsigned int)(y0 & 0xFFFF);
    unsigned int xy1 = ((unsigned int)(x1 & 0xFFFF) << 16) | (unsigned int)(y1 & 0xFFFF);
    return (int)syscall3(SYS_DRAW_LINE, (long)xy0, (long)xy1, (long)rgb);
}

/* Set the VESA caret style (0 = underline/line, 2 = flashing block).
 * Returns the previous style so callers can restore it on exit.  No-op
 * returning 0 in VGA-text mode. */
static inline unsigned int sys_set_caret_style(unsigned int style)
{
    return (unsigned int)syscall1(SYS_CARET_STYLE, (long)style);
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

static inline int sys_pipe(int pipefd[2])
{
    return (int)syscall1(SYS_PIPE, (long)pipefd);
}

static inline int sys_dup2(int oldfd, int newfd)
{
    return (int)syscall2(SYS_DUP2, (long)oldfd, (long)newfd);
}

static inline int sys_dup(int oldfd)
{
    return (int)syscall1(SYS_DUP, (long)oldfd);
}

static inline int sys_stat(const char *path, struct stat *st)
{
    return (int)syscall2(SYS_STAT, (long)path, (long)st);
}

static inline int sys_fstat(int fd, struct stat *st)
{
    return (int)syscall2(SYS_FSTAT, (long)fd, (long)st);
}

/* readdir: returns 1 if `*de` was filled, 0 at end-of-directory, -1 on error. */
static inline int sys_readdir(const char *path, unsigned int idx, struct dirent *de)
{
    return (int)syscall3(SYS_READDIR, (long)path, (long)idx, (long)de);
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

/* POSIX-style fcntl.  Currently supported: F_GETFL (cmd=3) returns the
 * current fd flags; F_SETFL (cmd=4) replaces them (the only meaningful
 * bit today is O_NONBLOCK on stdin).  Returns 0/flags on success,
 * negative errno on error. */
static inline int sys_fcntl(int fd, int cmd, long arg)
{
    return (int)syscall3(SYS_FCNTL, (long)fd, (long)cmd, arg);
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

/* Get PCI device list as text. Returns bytes written. */
static inline int sys_pci_info(char *buf, unsigned int bufsz)
{
    return (int)syscall2(SYS_PCI_INFO, (long)buf, (long)bufsz);
}

/* Get active Ethernet netdev info as text. Returns bytes written. */
static inline int sys_net_info(char *buf, unsigned int bufsz)
{
    return (int)syscall2(SYS_NET_INFO, (long)buf, (long)bufsz);
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

/* POSIX-numbered aliases for the four mutators above.  Same semantics;
 * libc wrappers (unlink/rmdir/rename) route through these standard
 * numbers so a future musl/uClibc port doesn't need a translation
 * shim. */
static inline int sys_unlink(const char *path)
{
    return (int)syscall1(SYS_UNLINK, (long)path);
}
static inline int sys_rmdir(const char *path)
{
    return (int)syscall1(SYS_RMDIR, (long)path);
}
static inline int sys_rename(const char *old_path, const char *new_path)
{
    return (int)syscall2(SYS_RENAME, (long)old_path, (long)new_path);
}
/* mode is accepted but ignored (no permission model). */
static inline int sys_mkdir(const char *path, unsigned int mode)
{
    return (int)syscall2(SYS_MKDIR, (long)path, (long)mode);
}

/* Send signo to pid.  Returns 0 on success, -1 on error (no such pid
 * or invalid signo).  No permission model yet -- any task may signal
 * any other. */
static inline int sys_kill(int pid, int signo)
{
    return (int)syscall2(SYS_KILL, (long)pid, (long)signo);
}

/* Install a handler for signo.  Returns the previous handler, or
 * (sig_handler_t)-1 on error.  User-defined handlers are stored but
 * not yet invoked by the kernel (no ring-3 trampoline yet); SIG_DFL
 * and SIG_IGN take effect immediately. */
static inline sig_handler_t sys_signal(int signo, sig_handler_t h)
{
    return (sig_handler_t)(unsigned long)
        syscall2(SYS_SIGNAL, (long)signo, (long)(unsigned long)h);
}

/* --- Admin syscalls.  Privileged operations the kernel arbitrates;
 * see <kernel/admin.h> for return-code conventions.  Currently every
 * task is admin (no user model). --- */

/* Power; never return on success. */
static inline int sys_reboot(void)
{
    return (int)syscall1(SYS_REBOOT, 0);
}
static inline int sys_shutdown(void)
{
    return (int)syscall1(SYS_SHUTDOWN, 0);
}

/* Display.  NULL/empty `arg` queries current state. */
static inline int sys_setmode(const char *mode)
{
    return (int)syscall1(SYS_SETMODE, (long)mode);
}
static inline int sys_fgcol(const char *colour)
{
    return (int)syscall1(SYS_FGCOL, (long)colour);
}
static inline int sys_bgcol(const char *colour)
{
    return (int)syscall1(SYS_BGCOL, (long)colour);
}

/* Storage.  `target` for sys_umount may be NULL (= sole mount). */
static inline int sys_eject(void)
{
    return (int)syscall1(SYS_EJECT, 0);
}
static inline int sys_install(void)
{
    return (int)syscall1(SYS_INSTALL, 0);
}
static inline int sys_mount(const char *dev, const char *mnt)
{
    return (int)syscall2(SYS_MOUNT, (long)dev, (long)mnt);
}
static inline int sys_umount(const char *target)
{
    return (int)syscall1(SYS_UMOUNT, (long)target);
}
static inline int sys_mkfs(const char *dev, const char *fstype)
{
    return (int)syscall2(SYS_MKFS, (long)dev, (long)fstype);
}

/* Scheduler / runtime tuning.  `new_value < 0` queries; returns the
 * post-call value. */
static inline int sys_sched_quantum(int new_value)
{
    return (int)syscall1(SYS_SCHED_QUANTUM, (long)new_value);
}

/* Serial-mirror toggle (Linux-style runtime console=ttyS0 opt-in).
 * onoff: 1 = on, 0 = off, -1 = query. */
static inline int sys_verbose(int onoff)
{
    return (int)syscall1(SYS_VERBOSE, (long)onoff);
}

/* Read /etc/hostname (falls back to "makar") into buf, capped at size-1
 * bytes, NUL-terminated.  Returns strlen on success, -1 on bad args. */
static inline int sys_gethostname(char *buf, unsigned int size)
{
    return (int)syscall2(SYS_GETHOSTNAME, (long)buf, (long)size);
}

/* Copy the current session's username ("user" on live boots) into buf,
 * capped at size-1 bytes, NUL-terminated.  Returns strlen, -1 on bad args. */
static inline int sys_whoami(char *buf, unsigned int size)
{
    return (int)syscall2(SYS_WHOAMI, (long)buf, (long)size);
}

/* Status-bar reservation (statusbar.elf uses this).  cmd: 1=enable/reserve the
 * bottom row, 0=disable/free it, <0=query only.  Returns the enabled state. */
static inline int sys_statusbar(int cmd)
{
    return (int)syscall1(SYS_STATUSBAR, (long)cmd);
}
static inline int sys_statusbar_enabled(void) { return sys_statusbar(-1); }
static inline int sys_statusbar_set(int on)   { return sys_statusbar(on ? 1 : 0); }

/* App-tab routing.  sys_vt_open_app: open `path` in a named tab (switch if it
 * exists, else queue for makmux).  sys_vt_take_app: makmux drains one queued
 * path.  sys_vt_setname: name the calling task's VT tab. */
static inline int sys_vt_open_app(const char *path)
{
    return (int)syscall1(SYS_VT_OPEN_APP, (long)path);
}
static inline int sys_vt_take_app(char *buf, int cap)
{
    return (int)syscall2(SYS_VT_TAKE_APP, (long)buf, (long)cap);
}
static inline int sys_vt_setname(const char *name)
{
    return (int)syscall1(SYS_VT_SETNAME, (long)name);
}
static inline int sys_vt_getname(int slot, char *buf, int cap)
{
    return (int)syscall3(SYS_VT_GETNAME, (long)slot, (long)buf, (long)cap);
}

/* Rootfs usage in KiB (total + free).  Returns 0 on success, -1 if the rootfs
 * doesn't report usage (FAT32/ISO9660). */
static inline int sys_statfs(unsigned int *total_kb, unsigned int *free_kb)
{
    return (int)syscall2(SYS_STATFS, (long)total_kb, (long)free_kb);
}

/* Emit `[shell:ready vt=N]` on COM1 when g_serial_verbose is set.
 * Called by /apps/sh.elf before each prompt so the in-guest test drivers'
 * `wait_for_serial` syncpoint works identically for both shells. */
static inline void sys_shell_ready(void)
{
    syscall1(SYS_SHELL_READY, 0);
}

static inline unsigned int sys_cursor_pos(void)
{
    return (unsigned int)syscall0(SYS_CURSOR_POS);
}

static inline int sys_vt_enter(int loading)
{
    return (int)syscall1(SYS_VT_ENTER, (long)loading);
}

static inline int sys_vt_close(int pid)
{
    return (int)syscall1(SYS_VT_CLOSE, (long)pid);
}

static inline int sys_vt_open_request(void)
{
    return (int)syscall0(SYS_VT_OPEN_REQUEST);
}

static inline unsigned int sys_vt_state(void)
{
    return (unsigned int)syscall0(SYS_VT_STATE);
}

static inline int sys_vt_clock_request(void)
{
    return (int)syscall0(SYS_VT_CLOCK_REQUEST);
}

#endif
