/*
 * fd.c -- Per-task file descriptor table.
 *
 * Each task owns a fd_table_t with TASK_MAX_FDS slots.  fds 0/1/2 are
 * pre-bound to keyboard/vga/vga+serial so freshly-created tasks behave
 * like POSIX processes.  Higher fds are allocated by SYS_OPEN.
 *
 * FD_KIND_FILE slots hold the file in a kmalloc'd, growable buffer.
 * If the `dirty` bit is set when the slot is closed (whether via
 * SYS_CLOSE or via fd_table_destroy at task tear-down), the buffer is
 * flushed back to the VFS via vfs_write_file(e->path, ...).  The path
 * lives inline on the slot so the close-flush doesn't need a separate
 * lookup, and so fork's deep-copy continues to work with a single
 * memcpy(slot, slot, sizeof slot).
 */

#include <kernel/fd.h>
#include <kernel/heap.h>
#include <kernel/vfs.h>
#include <string.h>

/* Flush a single dirty FILE slot back to its origin path.  Returns 0 on
 * success, -1 on flush error.  Safe to call on non-FILE / non-dirty
 * slots (returns 0 with no side-effect). */
static int fd_flush_one(fd_entry_t *e)
{
    if (!e || e->kind != FD_KIND_FILE || !e->dirty || !e->data || e->path[0] == '\0')
        return 0;
    int rc = vfs_write_file(e->path, e->data, e->size);
    /* Whether or not the flush succeeded, the buffer's view is now
     * authoritative for the caller -- clear dirty so a stray re-close
     * (e.g. fd_table_destroy after a manual fd_close) doesn't retry. */
    e->dirty = 0;
    return (rc == 0) ? 0 : -1;
}

fd_table_t *fd_table_create_default(void)
{
    fd_table_t *tbl = (fd_table_t *)kmalloc(sizeof(*tbl));
    if (!tbl)
        return NULL;

    memset(tbl, 0, sizeof(*tbl));
    tbl->slots[0].kind = FD_KIND_KEYBOARD;
    tbl->slots[1].kind = FD_KIND_VGA;
    tbl->slots[2].kind = FD_KIND_VGA_SERIAL;
    return tbl;
}

/* Decrement the right end's refcount on a pipe-kind slot.  Free the ring
 * when both ends have hit zero.  Safe to call on slots already torn down. */
static void pipe_release(fd_entry_t *e)
{
    if (!e || e->kind != FD_KIND_PIPE || !e->pipe)
        return;
    if (e->pipe_is_writer) {
        if (e->pipe->refcount_w > 0) e->pipe->refcount_w--;
    } else {
        if (e->pipe->refcount_r > 0) e->pipe->refcount_r--;
    }
    if (e->pipe->refcount_r == 0 && e->pipe->refcount_w == 0) {
        kfree(e->pipe);
    }
    e->pipe = NULL;
}

void fd_table_destroy(fd_table_t *tbl)
{
    if (!tbl)
        return;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        fd_entry_t *e = &tbl->slots[i];
        if (e->kind == FD_KIND_FILE) {
            (void)fd_flush_one(e);   /* best-effort on tear-down */
            if (e->data)
                kfree(e->data);
        } else if (e->kind == FD_KIND_PIPE) {
            pipe_release(e);
        }
    }
    kfree(tbl);
}

fd_table_t *fd_table_clone(const fd_table_t *src)
{
    if (!src)
        return NULL;
    fd_table_t *t = (fd_table_t *)kmalloc(sizeof(*t));
    if (!t)
        return NULL;
    memcpy(t, src, sizeof(*t));

    /* Each FILE slot needs its own buffer so writes/seeks in the child
     * don't bleed back into the parent's view.  We deliberately also
     * clear `dirty` on the child copy -- the parent owns the eventual
     * flush, and a double-flush from both sides would race on FAT32's
     * directory entry.  This is the documented non-POSIX shortcut that
     * the open_file_t refactor (slice list, pipe(2)/dup(2)) will undo. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (t->slots[i].kind == FD_KIND_FILE &&
            t->slots[i].data && t->slots[i].capacity) {
            uint8_t *buf = (uint8_t *)kmalloc(t->slots[i].capacity);
            if (!buf) {
                for (int j = 0; j < i; j++) {
                    if (t->slots[j].kind == FD_KIND_FILE && t->slots[j].data)
                        kfree(t->slots[j].data);
                }
                kfree(t);
                return NULL;
            }
            memcpy(buf, t->slots[i].data, t->slots[i].size);
            t->slots[i].data  = buf;
            t->slots[i].dirty = 0;
        } else if (t->slots[i].kind == FD_KIND_FILE) {
            /* Defensive: a FILE slot with no buffer is malformed; sever
             * the alias to the parent's data so close-on-OOM-unwind
             * doesn't double-free anything. */
            t->slots[i].data     = NULL;
            t->slots[i].capacity = 0;
            t->slots[i].dirty    = 0;
        } else if (t->slots[i].kind == FD_KIND_PIPE && t->slots[i].pipe) {
            /* Pipes share state across fork -- bump the right end's
             * refcount so the ring survives until every clone closes. */
            if (t->slots[i].pipe_is_writer)
                t->slots[i].pipe->refcount_w++;
            else
                t->slots[i].pipe->refcount_r++;
        }
    }
    return t;
}

int fd_alloc(fd_table_t *tbl)
{
    if (!tbl)
        return -1;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (tbl->slots[i].kind == FD_KIND_NONE)
            return i;
    }
    return -1;
}

fd_entry_t *fd_get(fd_table_t *tbl, int fd)
{
    if (!tbl || fd < 0 || fd >= TASK_MAX_FDS)
        return NULL;
    if (tbl->slots[fd].kind == FD_KIND_NONE)
        return NULL;
    return &tbl->slots[fd];
}

int fd_close(fd_table_t *tbl, int fd)
{
    fd_entry_t *e = fd_get(tbl, fd);
    if (!e)
        return -1;
    int rc = 0;
    if (e->kind == FD_KIND_FILE) {
        rc = fd_flush_one(e);   /* propagates flush errors to the caller */
        if (e->data)
            kfree(e->data);
    } else if (e->kind == FD_KIND_PIPE) {
        pipe_release(e);
    }
    memset(e, 0, sizeof(*e));
    /* memset already zeroes e->kind (== FD_KIND_NONE). */
    return rc;
}
