/*
 * fd.c -- Per-task file descriptor table.
 *
 * Each task owns a fd_table_t with TASK_MAX_FDS slots. fds 0/1/2 are
 * pre-bound to keyboard/vga/vga+serial so freshly-created tasks behave
 * like POSIX processes. Higher fds are allocated by SYS_OPEN; the file
 * payload is read eagerly into a heap buffer (cap SYSCALL_FILE_MAX),
 * matching the pre-slice behaviour preserved by syscall.c.
 */

#include <kernel/fd.h>
#include <kernel/heap.h>
#include <string.h>

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

void fd_table_destroy(fd_table_t *tbl)
{
    if (!tbl)
        return;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (tbl->slots[i].kind == FD_KIND_FILE && tbl->slots[i].data)
            kfree(tbl->slots[i].data);
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
     * don't bleed back into the parent's view. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (t->slots[i].kind == FD_KIND_FILE &&
            t->slots[i].data && t->slots[i].size) {
            uint8_t *buf = (uint8_t *)kmalloc(t->slots[i].size);
            if (!buf) {
                for (int j = 0; j < i; j++) {
                    if (t->slots[j].kind == FD_KIND_FILE && t->slots[j].data)
                        kfree(t->slots[j].data);
                }
                kfree(t);
                return NULL;
            }
            memcpy(buf, t->slots[i].data, t->slots[i].size);
            t->slots[i].data = buf;
        } else if (t->slots[i].kind == FD_KIND_FILE) {
            /* Defensive: a FILE slot with no buffer is malformed; sever
             * the alias to the parent's data so close-on-OOM-unwind
             * doesn't double-free anything. */
            t->slots[i].data = NULL;
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
    if (e->kind == FD_KIND_FILE && e->data)
        kfree(e->data);
    e->kind = FD_KIND_NONE;
    e->data = NULL;
    e->size = 0;
    e->pos  = 0;
    return 0;
}
