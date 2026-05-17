#ifndef _KERNEL_FD_H
#define _KERNEL_FD_H

#include <stdint.h>
#include <stddef.h>

/*
 * Per-task file descriptor table.
 *
 * Each task owns a fixed-size slot array. fds 0/1/2 are pre-bound to
 * stdin (keyboard), stdout (VGA), stderr (VGA + serial) at task creation
 * time to mirror POSIX. Higher fds are allocated by SYS_OPEN.
 *
 * Files are read eagerly into a heap buffer (cap SYSCALL_FILE_MAX),
 * preserving the pre-slice behaviour. The lazy/streaming model belongs to
 * a future slice (musl libc port).
 */

#define TASK_MAX_FDS  16

typedef enum {
    FD_KIND_NONE       = 0,   /* slot is free                              */
    FD_KIND_KEYBOARD   = 1,   /* stdin: keyboard_getchar() on read         */
    FD_KIND_VGA        = 2,   /* stdout: VGA terminal                      */
    FD_KIND_VGA_SERIAL = 3,   /* stderr: VGA terminal + COM1               */
    FD_KIND_SERIAL     = 4,   /* COM1 only (write-only)                    */
    FD_KIND_FILE       = 5,   /* opened VFS file (eagerly buffered)        */
} fd_kind_t;

/* Per-fd flag bits, mirrored from Linux fcntl O_NONBLOCK.  Stored in
 * fd_entry_t.flags and consulted by SYS_READ on FD_KIND_KEYBOARD to
 * decide whether to block-yield or return -EAGAIN immediately when the
 * keyboard ring is empty. */
#define FD_FLAG_NONBLOCK  0x800u

typedef struct {
    fd_kind_t kind;
    /* file-kind state (zero/NULL for other kinds) */
    uint8_t  *data;     /* kmalloc'd buffer, owned by this slot      */
    uint32_t  size;     /* total bytes valid in data                  */
    uint32_t  pos;      /* current read/seek position                 */
    uint32_t  flags;    /* FD_FLAG_*; per-fd modes (e.g. O_NONBLOCK)  */
} fd_entry_t;

typedef struct fd_table {
    fd_entry_t slots[TASK_MAX_FDS];
} fd_table_t;

/*
 * fd_table_create_default -- allocate a new table with fds 0/1/2 pre-bound
 * (stdin=keyboard, stdout=vga, stderr=vga+serial). Returns NULL on OOM.
 */
fd_table_t *fd_table_create_default(void);

/*
 * fd_table_destroy -- close every open fd, free file buffers, free the table.
 * Safe to call with NULL.
 */
void fd_table_destroy(fd_table_t *tbl);

/*
 * fd_alloc -- return the lowest free fd index, or -1 if the table is full.
 * The slot is left as FD_KIND_NONE; caller fills in kind/data/size/pos.
 */
int fd_alloc(fd_table_t *tbl);

/*
 * fd_get -- return the slot for fd, or NULL if fd is out of range or unused.
 */
fd_entry_t *fd_get(fd_table_t *tbl, int fd);

/*
 * fd_close -- close fd, free file buffer if present, clear the slot.
 * Returns 0 on success, -1 if fd is invalid.
 */
int fd_close(fd_table_t *tbl, int fd);

#endif /* _KERNEL_FD_H */
