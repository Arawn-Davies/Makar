#ifndef _MAKAR_ABI_H
#define _MAKAR_ABI_H

/*
 * makar_abi.h -- the typed syscall ABI shared by the kernel (<kernel/syscall.h>)
 * and userspace (src/userspace/syscall.h): the flags + structs that both sides
 * must agree on byte-for-byte.  Was duplicated and "must match" by hand.
 *
 * Plain integer types only (unsigned int / unsigned short / int) -- NOT the
 * <stdint.h> uint32_t/uint16_t -- so the header is usable under the userspace
 * -nostdinc build.  On i386 these are layout-identical to the fixed-width types
 * the kernel previously used (int==int32_t, short==int16_t).  Per-struct guards
 * let this coexist with either side's leftover definitions.
 */

/* clockid_t values for SYS_CLOCK_GETTIME. */
#define CLOCK_REALTIME  0   /* Wall clock from CMOS RTC, seconds since 1970 */
#define CLOCK_MONOTONIC 1   /* PIT-derived uptime, never jumps backwards    */

#ifndef _MAKAR_STRUCT_TIMEVAL_DEFINED
#define _MAKAR_STRUCT_TIMEVAL_DEFINED
struct timeval  { int tv_sec; int tv_usec; };
#endif
#ifndef _MAKAR_STRUCT_TIMESPEC_DEFINED
#define _MAKAR_STRUCT_TIMESPEC_DEFINED
struct timespec { int tv_sec; int tv_nsec; };
#endif

/* fcntl cmd values (Linux i386 ABI subset). */
#define F_GETFL  3
#define F_SETFL  4

/* open() flags -- low 2 bits are the access mode; the rest are status flags.
 * Values mirror the Linux i386 ABI. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3       /* mask: (flags & O_ACCMODE) is the access mode */
#define O_CREAT   0100    /* create file if it doesn't exist              */
#define O_TRUNC   01000   /* truncate to zero length on open              */
#define O_APPEND  02000   /* writes always land at end                    */

/* one screen cell passed to SYS_PUTCH_AT; clr is a VGA attribute byte. */
#ifndef _MAKAR_TTY_CELL_DEFINED
#define _MAKAR_TTY_CELL_DEFINED
typedef struct { unsigned char col, row, ch, clr; } tty_cell_t;
#endif

/* struct stat -- Linux i386 layout (the 32-bit stat, not stat64).  Fields the
 * filesystems can't populate are zero-filled by SYS_STAT / SYS_FSTAT. */
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

/* file-type bits in st_mode (octal, matching Linux/POSIX). */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000

/* index-addressed dirent for SYS_READDIR.  Stable shape for both sides; the
 * name buffer is sized for the longest VFS path component. */
#define DIRENT_NAME_MAX 256
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#ifndef _MAKAR_STRUCT_DIRENT_DEFINED
#define _MAKAR_STRUCT_DIRENT_DEFINED
struct dirent {
    unsigned int   d_ino;       /* synthetic; same FNV-1a as stat */
    unsigned char  d_type;      /* DT_REG / DT_DIR / DT_UNKNOWN    */
    unsigned char  __pad[3];
    char           d_name[DIRENT_NAME_MAX];
};
#endif

/* ------------------------------------------------------------------------
 * Installer ABI -- shared by the in-kernel install engine (installer.c) and the
 * GUI installer client (mxinstall.elf), driven via SYS_INSTALL_EXEC.
 * ------------------------------------------------------------------------ */
#define INSTALL_FS_EXT2    0
#define INSTALL_FS_FAT32   1
#define INSTALL_MAX_DRIVES 4

#ifndef _MAKAR_STRUCT_INSTALL_DEFINED
#define _MAKAR_STRUCT_INSTALL_DEFINED
typedef struct {
    unsigned char drive;          /* IDE drive index (target) */
    unsigned char fs;             /* INSTALL_FS_EXT2 / INSTALL_FS_FAT32 (data part) */
    unsigned char install_docs;   /* copy /docs */
    unsigned char install_src;    /* copy /src  */
    char          hostname[64];   /* "" -> "makar" */
    char          autologin[64];  /* "" -> none */
    char          root_pw[128];   /* "" -> leave root password unset */
    char          user_name[64];  /* "" -> no extra user */
    char          user_pw[128];
} install_params_t;

typedef struct {
    int          done;            /* 1 once every tree has been copied */
    unsigned int files;           /* files copied so far */
    unsigned int total;           /* total files to copy (for the % bar) */
    char         current[64];     /* name of the file just copied */
} install_progress_t;

typedef struct {
    unsigned char index;          /* IDE drive index -> install_params_t.drive */
    unsigned int  size_mib;
    char          model[40];
} install_drive_t;
#endif

/* ------------------------------------------------------------------------
 * BSD sockets ABI (subset) -- AF_INET / SOCK_STREAM only, shared by the kernel
 * socket layer (SYS_SOCKET / SYS_CONNECT) and the userspace wrappers.  The
 * sockaddr_in layout matches Linux i386 so the shape is familiar and a future
 * socketcall(102) shim for musl-linked binaries (e.g. Dropbear) can reuse it.
 * ------------------------------------------------------------------------ */
#define AF_INET      2
#define SOCK_STREAM  1
#define IPPROTO_TCP  6

#ifndef _MAKAR_STRUCT_SOCKADDR_IN_DEFINED
#define _MAKAR_STRUCT_SOCKADDR_IN_DEFINED
struct in_addr { unsigned int s_addr; };         /* IPv4, network byte order */
struct sockaddr_in {
    unsigned short sin_family;                     /* AF_INET                  */
    unsigned short sin_port;                       /* port, network byte order */
    struct in_addr sin_addr;                       /* address, network order   */
    unsigned char  sin_zero[8];                    /* pad to 16 bytes          */
};
#endif

#endif /* _MAKAR_ABI_H */
