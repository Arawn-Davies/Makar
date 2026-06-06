#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>
#include <kernel/isr.h>

/* Syscall NUMBERS live in the shared ABI header (one source of truth for the
 * kernel + userspace; Linux-uapi style).  This header keeps only the typed ABI
 * bits that need <stdint.h>: clockids, struct timeval/stat/dirent, fcntl/open
 * flags, tty_cell_t, and the kernel-side syscall_init/dispatch decls. */
#include <makar_syscalls.h>

/* clockid_t values for SYS_CLOCK_GETTIME. */
#define CLOCK_REALTIME  0   /* Wall clock from CMOS RTC, seconds since 1970     */
#define CLOCK_MONOTONIC 1   /* PIT-derived uptime, never jumps backwards        */

/* Timeval / timespec -- Linux i386 layouts.  Per-struct guards so this
 * header coexists with src/userspace/syscall.h when the in-OS TCC
 * kernel rebuild includes both (via kernel/timer.h -> stdio.h -> the
 * userspace syscall.h chain). */
#ifndef _MAKAR_STRUCT_TIMEVAL_DEFINED
#define _MAKAR_STRUCT_TIMEVAL_DEFINED
struct timeval  { int32_t tv_sec; int32_t tv_usec; };
#endif
#ifndef _MAKAR_STRUCT_TIMESPEC_DEFINED
#define _MAKAR_STRUCT_TIMESPEC_DEFINED
struct timespec { int32_t tv_sec; int32_t tv_nsec; };
#endif

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
 * limit (16 MiB) before SYS_WRITE returns -1 (EFBIG).  Kernel heap is 32 MiB
 * (see HEAP_MAX in kernel/heap.h), so two simultaneously-open large files
 * still leave the kernel room to operate.  16 MiB is enough for tcc.c + its
 * emitted ELF on a self-compile attempt. */
#define SYSCALL_FILE_MAX     (16u * 1024u * 1024u)
/* Initial heap allocation for a freshly-created (O_CREAT) or O_TRUNC'd fd.
 * Subsequent SYS_WRITEs grow geometrically (doubling). */
#define SYSCALL_FILE_INITIAL (4u * 1024u)

/* -------------------------------------------------------------------------
 * struct stat -- Linux i386 layout (the 32-bit `struct stat`, not stat64).
 * Fields we cannot populate from Makar's filesystems are zero-filled by the
 * SYS_STAT / SYS_FSTAT implementation.
 * ---------------------------------------------------------------------- */
#ifndef _MAKAR_STRUCT_STAT_DEFINED
#define _MAKAR_STRUCT_STAT_DEFINED
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
#endif

/* Index-addressed dirent for SYS_READDIR(141).  Stable shape for both
 * kernel and userspace headers; the name buffer is sized for the longest
 * VFS path component (VFS_PATH_MAX is the full-path cap). */
#define DIRENT_NAME_MAX 256
#ifndef _MAKAR_STRUCT_DIRENT_DEFINED
#define _MAKAR_STRUCT_DIRENT_DEFINED
struct dirent {
    uint32_t d_ino;             /* synthetic; same FNV-1a as stat */
    uint8_t  d_type;            /* DT_REG / DT_DIR / DT_UNKNOWN */
    uint8_t  __pad[3];
    char     d_name[DIRENT_NAME_MAX];
};
#endif
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
