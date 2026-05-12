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
#define SYS_READ       3    /* ssize_t read(int fd, void *buf, size_t len)      */
#define SYS_WRITE      4    /* ssize_t write(int fd, const void *buf, size_t)   */
#define SYS_OPEN       5    /* int open(const char *path, int flags)            */
#define SYS_CLOSE      6    /* int close(int fd)                                */
#define SYS_LSEEK      19   /* off_t lseek(int fd, off_t offset, int whence)   */
#define SYS_BRK        45   /* void *brk(void *addr)                            */
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

/* open() flags */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2

/* Well-known file descriptors */
#define FD_STDIN    0
#define FD_STDOUT   1
#define FD_STDERR   2

/* Maximum size of a file that can be opened via SYS_OPEN (64 KiB). */
#define SYSCALL_FILE_MAX  (64u * 1024u)

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

/*
 * syscall_reset_fds - close all open file descriptors.
 *
 * Called by elf_exec() before launching a new process so that a fresh exec
 * does not inherit stale file handles from the previous run.
 */
void syscall_reset_fds(void);

#endif /* _KERNEL_SYSCALL_H */
