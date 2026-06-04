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
#define SYS_UNLINK     10   /* int unlink(const char *path)  -- alias of 208    */
#define SYS_EXECVE     11   /* int execve(const char *path, char *const argv[], char *const envp[]) */
#define SYS_CHDIR      12   /* int chdir(const char *path) -- set calling task's cwd */
#define SYS_LSEEK      19   /* off_t lseek(int fd, off_t offset, int whence)   */
#define SYS_GETPID     20   /* pid_t getpid(void)                               */
#define SYS_DUP        41   /* int dup(int oldfd) -- lowest free fd, Linux i386 */
#define SYS_PIPE       42   /* int pipe(int pipefd[2]) -- alloc reader+writer  */
#define SYS_DUP2       63   /* int dup2(int oldfd, int newfd) -- Linux i386 ABI*/
#define SYS_WAIT4      114  /* pid_t wait4(pid, int *status, int options, void *rusage) */
#define SYS_KILL       37   /* int kill(int pid, int signo)                     */
#define SYS_RENAME     38   /* int rename(const char *old, const char *new)     */
#define SYS_MKDIR      39   /* int mkdir(const char *path, mode_t mode)         */
#define SYS_RMDIR      40   /* int rmdir(const char *path)                      */
#define SYS_BRK        45   /* void *brk(void *addr)                            */
#define SYS_MUNMAP     91   /* int munmap(void *addr, size_t len)               */
#define SYS_MMAP2     192   /* void *mmap2(addr,len,prot,flags,fd,pgoff) -- anon only */
/* musl/Linux process-startup syscalls (hosted toolchain bring-up) */
#define SYS_IOCTL          54   /* int ioctl(fd, req, ...) -- stub (-ENOTTY)        */
#define SYS_WRITEV        146   /* ssize_t writev(fd, const struct iovec*, int)     */
#define SYS_RT_SIGPROCMASK 175  /* int rt_sigprocmask(how, set, oldset, sigsetsize) -- stub */
#define SYS_FUTEX         240   /* int futex(...) -- no-op (single-threaded) */
#define SYS_SET_THREAD_AREA 243 /* int set_thread_area(struct user_desc*) -- TLS    */
#define SYS_EXIT_GROUP    252   /* void exit_group(int status) -- == exit           */
#define SYS_SET_TID_ADDRESS 258 /* int set_tid_address(int *tidptr) -> tid          */
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

/*
 * Admin syscalls (219..229).
 *
 * Each gates on task_is_admin() (always true today; future login model
 * tightens this).  Side-effect semantics + arg validation live in
 * kernel/admin.h helpers; the dispatcher is a thin marshaller.
 *
 * String args are read directly from the calling task's address space
 * (the caller's PD is loaded when int 0x80 runs, so a kernel-mode
 * dereference reaches user memory).
 *
 * Return:
 *   >= 0  success or current state (e.g. SYS_SCHED_QUANTUM returns ticks)
 *   -1    permission denied OR generic failure
 *   <-1   admin-helper-specific negative errno
 */
#define SYS_REBOOT         219  /* int reboot(void) - noreturn on success */
#define SYS_SHUTDOWN       220  /* int shutdown(void) - noreturn on success */
#define SYS_SETMODE        221  /* int setmode(const char *mode) */
#define SYS_FGCOL          222  /* int fgcol(const char *colour) */
#define SYS_BGCOL          223  /* int bgcol(const char *colour) */
#define SYS_EJECT          224  /* int eject(void) */
#define SYS_MOUNT          225  /* int mount(const char *dev, const char *mnt) */
#define SYS_UMOUNT         226  /* int umount(const char *target) -- NULL ok */
#define SYS_MKFS           227  /* int mkfs(const char *dev, const char *fstype) */
#define SYS_SCHED_QUANTUM  228  /* int sched_quantum(int new_value) -- <0 = query */
#define SYS_VERBOSE        229  /* int verbose(int onoff) -- 1/0/-1 */
#define SYS_SHELL_READY    231  /* void shell_ready_marker(void) - emit the
                                 * `[shell:ready vt=N]` sync marker on COM1
                                 * if g_serial_verbose; no-op otherwise.
                                 * Userspace shell calls this before each
                                 * prompt so the in-guest test drivers'
                                 * serial sync keeps working unchanged. */
#define SYS_CURSOR_POS     232  /* uint32_t cursor_pos(void) -> (col<<16)|row */
#define SYS_VT_ENTER       233  /* int vt_enter(int focus_new) - attach this task
                                 * as a makmux shell on the next VT slot. */
#define SYS_VT_CLOSE       234  /* int vt_close(pid_t pid) - close task's VT */
#define SYS_VT_OPEN_REQUEST 235 /* int vt_open_request(void) - consume Alt+T */
#define SYS_VT_STATE       236 /* uint32_t vt_state(void) -> active<<16 | mask */
#define SYS_VT_CLOCK_REQUEST 237 /* int vt_clock_request(void) - consume Alt+F5 */
#define SYS_PCI_INFO         239 /* int pci_info(char *buf, uint32_t bufsz) - render
                                  * pci_devices[] as text.  Returns bytes written. */
#define SYS_INSTALL          238 /* int install(void) - run the Limine installer TUI;
                                  * blocks the calling task until the installer
                                  * exits.  Returns 0 on success, -1 otherwise. */
#define SYS_GETHOSTNAME    230  /* int gethostname(char *buf, size_t size) - read
                                 * /etc/hostname (or fallback "makar"), copy up
                                 * to size-1 bytes, NUL-terminate.  Returns
                                 * strlen on success, -1 if buf invalid. */
#define SYS_WHOAMI         247  /* int whoami(char *buf, size_t size) - copy the
                                 * current session's username (auth_current_user,
                                 * "user" on live boots) up to size-1 bytes,
                                 * NUL-terminate.  Returns strlen, -1 if invalid. */
#define SYS_STATUSBAR      241  /* int statusbar(int cmd) - reserve/free the bottom
                                 * status row (mechanism for userspace statusbar.elf).
                                 * cmd: 1=enable (reserve), 0=disable (free), <0=query.
                                 * Returns the resulting enabled state (0/1). */
#define SYS_VT_OPEN_APP    242  /* int vt_open_app(const char *path) - open an app
                                 * in a named tab: switch to it if it exists, else
                                 * queue for makmux to spawn.  Returns 1/0. */
#define SYS_VT_TAKE_APP    248  /* int vt_take_app(char *buf, int cap) - makmux
                                 * drains one queued app path.  Returns 1/0. */
#define SYS_VT_SETNAME     244  /* int vt_setname(const char *name) - name the
                                 * calling task's VT tab (shown in the status bar). */
#define SYS_VT_GETNAME     245  /* int vt_getname(int slot, char *buf, int cap) -
                                 * copy slot's tab name (empty for unnamed VT
                                 * shells).  Returns strlen.  Used by statusbar. */
#define SYS_STATFS         246  /* int statfs(uint32_t *total_kb, uint32_t *free_kb)
                                 * rootfs usage in KiB (ext2 only).  Returns 0/-1. */

/* Microkernel IPC (MINIX-style synchronous message passing).  ebx = endpoint
 * pid (or IPC_ANY for recv), ecx = ipc_msg_t*.  See kernel/ipc.h. */
#define SYS_IPC_SEND       249  /* int ipc_send(int dst, const ipc_msg_t *)        */
#define SYS_IPC_RECV       250  /* int ipc_recv(int from, ipc_msg_t *)             */
#define SYS_IPC_SENDREC    251  /* int ipc_sendrec(int dst, ipc_msg_t *)           */

/* Back to Linux i386 ABI numbers for the next set. */
#define SYS_FCNTL      55   /* int fcntl(int fd, int cmd, int arg) - F_GETFL/F_SETFL */
#define SYS_GETPPID    64   /* pid_t getppid(void)                              */
#define SYS_GETTIMEOFDAY 78 /* int gettimeofday(struct timeval *, void *)       */
#define SYS_CLOCK_GETTIME 265 /* int clock_gettime(clockid_t, struct timespec *)*/

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
