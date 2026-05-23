#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>
#include <kernel/isr.h>

/*
 * Syscall numbers - Linux i386 ABI subset.
 *
 * Registers (int 0x80 calling convention):
 *   EAX = syscall number
 *   EBX = arg1, ECX = arg2, EDX = arg3
 *   Return value written back to EAX (negative errno on error).
 */
#define SYS_EXIT       1    /* void exit(int status)                            */
#define SYS_FORK       2    /* pid_t fork(void) -- COW clone of caller          */
#define SYS_READ       3    /* ssize_t read(int fd, void *buf, size_t len)      */
#define SYS_WRITE      4    /* ssize_t write(int fd, const void *buf, size_t)   */
#define SYS_OPEN       5    /* int open(const char *path, int flags)            */
#define SYS_CLOSE      6    /* int close(int fd)                                */
#define SYS_EXECVE     11   /* int execve(const char *path, char *const argv[], char *const envp[]) */
#define SYS_LSEEK      19   /* off_t lseek(int fd, off_t offset, int whence)   */
#define SYS_WAIT4      114  /* pid_t wait4(pid, int *status, int options, void *rusage) */
#define SYS_KILL       37   /* int kill(int pid, int signo)                     */
#define SYS_BRK        45   /* void *brk(void *addr)                            */
#define SYS_SIGNAL     48   /* sig_handler_t signal(int signo, sig_handler_t)   */
#define SYS_STAT      106   /* int stat(const char *path, struct stat *st)      */
#define SYS_FSTAT     108   /* int fstat(int fd, struct stat *st)               */
#define SYS_READDIR   141   /* int readdir(path, idx, struct dirent *)          */
#define SYS_SIGRETURN  119  /* void sigreturn(void) -- not for direct use      */
#define SYS_DEBUG      100  /* void debug(uint32_t cp)      [Makar ext]         */
#define SYS_YIELD      158  /* void sched_yield(void)                           */
/* Makar display/input extensions (200+) */
#define SYS_GETKEY     200  /* int getkey(void)  - raw single-char keyboard     */
#define SYS_PUTCH_AT   201  /* int putch_at(tty_cell_t*, uint32_t n)            */
#define SYS_SET_CURSOR 202  /* void set_cursor(uint32_t col, uint32_t row)      */
#define SYS_TTY_CLEAR  203  /* void tty_clear(uint8_t clr)                      */
#define SYS_TERM_SIZE  204  /* uint32_t term_size() → (cols<<16)|rows           */
#define SYS_WRITE_FILE 205  /* int write_file(path, buf, len)                   */
#define SYS_LS_DIR     206  /* int ls_dir(path, buf, bufsz) → bytes written     */
#define SYS_DISK_INFO    207  /* int disk_info(buf, bufsz) → bytes written        */
#define SYS_DELETE_FILE  208  /* int delete_file(path)                            */
#define SYS_RENAME_FILE  209  /* int rename_file(old_path, new_path)              */
#define SYS_DELETE_DIR   210  /* int delete_dir(path)                             */
#define SYS_WRITE_SERIAL 211  /* ssize_t write_serial(const void *buf, size_t len)*/
#define SYS_KEYBOARD_RAW 212  /* void keyboard_raw(int on)   - see keyboard.h     */
#define SYS_SHELL_CLEAR  213  /* void shell_clear(void)     - same as `clear`     */
#define SYS_UPTIME       214  /* uint32_t uptime_ticks(void) - 100 Hz timer ticks */
#define SYS_GETCWD       215  /* int getcwd(char *buf, size_t size) - copy task cwd */
#define SYS_FB_INFO      216  /* uint32_t fb_info(void) - VESA only; 0 if VGA-only.
                                * Returns (width << 16) | height when available. */
#define SYS_DRAW_LINE    217  /* int draw_line(uint32_t xy0, uint32_t xy1,
                                *               uint32_t rgb)
                                * Bresenham line in framebuffer pixels.  Coords
                                * packed (x << 16) | y.  Returns 0 on success,
                                * (uint32_t)-1 if pixel mode unavailable.
                                * Clipped to drawable area (status row excluded). */
#define SYS_CARET_STYLE  218  /* uint32_t caret_style(uint32_t style) - set the
                                * VESA caret style (0=line, 2=flashing block),
                                * returns the previous style.  No-op (returns 0)
                                * in VGA-text mode.  Used by vix.elf. */

/* Back to Linux i386 ABI numbers for the next set. */
#define SYS_FCNTL      55   /* int fcntl(int fd, int cmd, int arg) - F_GETFL/F_SETFL */

/* fcntl cmd values (Linux i386 ABI subset). */
#define F_GETFL         3
#define F_SETFL         4

/*
 * tty_cell_t - one screen cell passed to SYS_PUTCH_AT.
 * clr is a standard VGA attribute byte: fg = bits[3:0], bg = bits[6:4].
 */
typedef struct {
    uint8_t col;
    uint8_t row;
    uint8_t ch;
    uint8_t clr;
} tty_cell_t;

/* open() flags -- low 2 bits are access mode; the rest are status flags.
 * Values mirror the Linux i386 ABI so a future uClibc-ng / musl port doesn't
 * need a translation shim. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3       /* mask: (flags & O_ACCMODE) is the access mode */
#define O_CREAT     0100    /* create file if it doesn't exist              */
#define O_TRUNC     01000   /* truncate to zero length on open              */
#define O_APPEND    02000   /* writes always land at e->size                */

/* Well-known file descriptors */
#define FD_STDIN    0
#define FD_STDOUT   1
#define FD_STDERR   2

/* Maximum size of a regular file backed by an in-memory FD_KIND_FILE buffer.
 * Reads cap the eager-load here; writes may grow the buffer up to this hard
 * limit (8 MiB) before SYS_WRITE returns -1 (EFBIG).  Kernel heap is roughly
 * 16 MiB total, so a single open file can hold a realistic TCC TU + output
 * without starving everything else. */
#define SYSCALL_FILE_MAX     (8u * 1024u * 1024u)
/* Initial heap allocation for a freshly-created (O_CREAT) or O_TRUNC'd fd.
 * Subsequent SYS_WRITEs grow geometrically (doubling). */
#define SYSCALL_FILE_INITIAL (4u * 1024u)

/* -------------------------------------------------------------------------
 * struct stat -- Linux i386 layout (the 32-bit `struct stat`, not stat64).
 * Fields we cannot populate from Makar's filesystems are zero-filled by the
 * SYS_STAT / SYS_FSTAT implementation.
 * ---------------------------------------------------------------------- */
struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};

/* Index-addressed dirent for SYS_READDIR(141).  Stable shape for both
 * kernel and userspace headers; the name buffer is sized for the longest
 * VFS path component (VFS_PATH_MAX is the full-path cap). */
#define DIRENT_NAME_MAX 256
struct dirent {
    uint32_t d_ino;             /* synthetic; same FNV-1a as stat */
    uint8_t  d_type;            /* DT_REG / DT_DIR / DT_UNKNOWN */
    uint8_t  __pad[3];
    char     d_name[DIRENT_NAME_MAX];
};
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

/* File type bits in st_mode (octal, matching Linux/POSIX). */
#define S_IFMT      0170000
#define S_IFREG     0100000
#define S_IFDIR     0040000
#define S_IFCHR     0020000
#define S_IFBLK     0060000
#define S_IFIFO     0010000
#define S_IFLNK     0120000

/*
 * g_ring3_last_cp - last SYS_DEBUG checkpoint value received from ring-3.
 * Reset to 0 before launching a ring-3 test task; read after it exits.
 */
extern volatile uint32_t g_ring3_last_cp;

/*
 * syscall_init - register int 0x80 in the interrupt-handler table.
 *
 * Must be called after init_descriptor_tables() and tasking_init().
 */
void syscall_init(void);

/*
 * syscall_dispatch - the int 0x80 C handler; exposed for in-kernel testing.
 */
void syscall_dispatch(registers_t *regs);

#endif /* _KERNEL_SYSCALL_H */
