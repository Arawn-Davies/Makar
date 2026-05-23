#ifndef _KERNEL_FD_H
#define _KERNEL_FD_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>     /* VFS_PATH_MAX */

/*
 * Per-task file descriptor table.
 *
 * Each task owns a fixed-size slot array. fds 0/1/2 are pre-bound to
 * stdin (keyboard), stdout (VGA), stderr (VGA + serial) at task creation
 * time to mirror POSIX. Higher fds are allocated by SYS_OPEN.
 *
 * FD_KIND_FILE slots hold the file in a kmalloc'd, growable heap buffer.
 * Read-mode opens eager-load the whole file (capped at SYSCALL_FILE_MAX).
 * Write-mode opens start at SYSCALL_FILE_INITIAL and grow via krealloc on
 * demand; the buffer is flushed back via vfs_write_file on close when the
 * `dirty` bit is set.  The path the fd was opened against is kept inline
 * on the slot so close-flush can name the destination without a second
 * lookup; the open_file_t refactor that would deduplicate this lives on
 * the slice list for pipe(2)/dup(2).
 */

#define TASK_MAX_FDS  16

typedef enum {
    FD_KIND_NONE       = 0,   /* slot is free                              */
    FD_KIND_KEYBOARD   = 1,   /* stdin: keyboard_getchar() on read         */
    FD_KIND_VGA        = 2,   /* stdout: VGA terminal                      */
    FD_KIND_VGA_SERIAL = 3,   /* stderr: VGA terminal + COM1               */
    FD_KIND_SERIAL     = 4,   /* COM1 only (write-only)                    */
    FD_KIND_FILE       = 5,   /* opened VFS file (eagerly buffered)        */
    FD_KIND_BLOCKDEV   = 6,   /* /dev block device (no buffer; sector I/O) */
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
    uint32_t  size;     /* logical EOF in data (also device size for BLOCKDEV) */
    uint32_t  capacity; /* FILE: bytes allocated in *data; size <= capacity */
    uint32_t  pos;      /* current read/seek position                 */
    uint32_t  flags;    /* FD_FLAG_*; per-fd modes (e.g. O_NONBLOCK)  */
    int       dev_node; /* FD_KIND_BLOCKDEV: devfs node index         */
    uint8_t   dirty;    /* FILE: data differs from on-disk; flush on close */
    uint8_t   writable; /* FILE: opened with O_WRONLY or O_RDWR        */
    uint8_t   append;   /* FILE: O_APPEND -- force pos = size before write */
    char      path[VFS_PATH_MAX];  /* FILE: absolute path for close-flush */
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
 * fd_table_clone -- deep-copy a table for fork().  Each FILE-kind slot's
 * data buffer is kmalloc'd separately so parent and child have independent
 * seek/write state.  This diverges from POSIX (which shares one open-file
 * description across forks); a refactor to refcounted open_file_t is on
 * the slice list for pipe(2)/dup(2).  Returns NULL on OOM (and unwinds
 * any partially-cloned slots).
 */
fd_table_t *fd_table_clone(const fd_table_t *src);

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
