/*
 * tmpfs.c - synthetic, writable in-RAM directories.  See kernel/tmpfs.h.
 *
 * Flat in-RAM scratch directory.  File records and payload buffers are
 * allocated from the kernel heap on demand; later writes overwrite the
 * buffer wholesale (no ring, unlike logfs).  The intended consumer is
 * tcc(1) producing an ELF in /tmp before the shell `exec`s it.
 *
 * Multi-instance: each record carries a namespace id `ns` so several tmpfs
 * mounts can coexist without aliasing.  /tmp is ns 0; a read-only live root
 * adds writable home overlays (/root, /home/user) as separate namespaces, so
 * `/root/.mxrc` and `/tmp/.mxrc` never collide even though their relative
 * paths are identical.
 */

#include <kernel/tmpfs.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <string.h>
#include <stddef.h>

#define TMPFS_NAME_MAX    256   /* room for nested-path-as-flat-name */
#define TMPFS_FILE_MAX    (16u * 1024u * 1024u)

typedef struct tmpfile {
    int      ns;               /* namespace (mount instance) this entry lives in */
    char     name[TMPFS_NAME_MAX];
    char    *buf;
    uint32_t cap;
    uint32_t len;
    struct tmpfile *next;
} tmpfile_t;

static tmpfile_t *s_files;

/* Strip the leading '/' from a tmpfs-relative path.  Returns the rest as
 * a single flat name; inner slashes are preserved verbatim (tmpfs has no
 * real subdirectories -- "/foo/bar.o" and "/foo_bar.o" are two distinct
 * entries).  Returns NULL for "/" or malformed paths. */
static const char *leaf_name(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0') return NULL;
    return path + 1;
}

/* Find an existing file by (namespace, bare name). */
static tmpfile_t *find(int ns, const char *name)
{
    for (tmpfile_t *f = s_files; f; f = f->next)
        if (f->ns == ns && strcmp(f->name, name) == 0) return f;
    return NULL;
}

/* Find or lazily create a heap-backed file record by (ns, bare name).
 * Returns NULL if the name is too long or the heap allocation fails. */
static tmpfile_t *find_or_create(int ns, const char *name)
{
    tmpfile_t *f = find(ns, name);
    if (f) return f;
    if (strlen(name) >= TMPFS_NAME_MAX) return NULL;

    f = (tmpfile_t *)kmalloc(sizeof(tmpfile_t));
    if (!f) return NULL;
    f->ns = ns;
    strncpy(f->name, name, TMPFS_NAME_MAX - 1);
    f->name[TMPFS_NAME_MAX - 1] = '\0';
    f->buf  = NULL;
    f->cap  = 0;
    f->len  = 0;
    f->next = s_files;
    s_files = f;
    return f;
}

/* -------------------------------------------------------------------------
 * Public VFS-facing API (paths are tmpfs-relative, leading '/').
 * ---------------------------------------------------------------------- */

long tmpfs_read(int ns, const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    const char *name = leaf_name(path);
    tmpfile_t  *f    = name ? find(ns, name) : NULL;
    if (!f) { if (out_sz) *out_sz = 0; return -1; }

    uint32_t n = (f->len < bufsz) ? f->len : bufsz;
    if (buf && n > 0)
        memcpy(buf, f->buf, n);
    if (out_sz) *out_sz = n;
    return 0;
}

long tmpfs_write(int ns, const char *path, const void *buf, uint32_t len)
{
    const char *name = leaf_name(path);
    if (!name) return -1;
    tmpfile_t *f = find_or_create(ns, name);
    if (!f) return -1;
    if (len > TMPFS_FILE_MAX) return -1;       /* refuse oversized write */
    if (len > f->cap) {
        char *newbuf = (char *)krealloc(f->buf, len);
        if (!newbuf) return -1;
        f->buf = newbuf;
        f->cap = len;
    }
    if (len > 0 && buf)
        memcpy(f->buf, buf, len);
    f->len = len;
    return (long)len;
}

int tmpfs_file_exists(int ns, const char *path)
{
    const char *name = leaf_name(path);
    return (name && find(ns, name)) ? 1 : 0;
}

int tmpfs_ls(int ns, const char *path)
{
    /* flat namespace: only "/" lists; "/<name>" is a file, not a directory. */
    if (path && path[0] == '/' && path[1] != '\0') {
        t_writestring("ls: ");
        t_writestring(path);
        t_writestring(tmpfs_file_exists(ns, path) ? ": Not a directory\n"
                                                  : ": No such entry\n");
        return -1;
    }
    for (tmpfile_t *f = s_files; f; f = f->next) {
        if (f->ns != ns) continue;
        t_writestring(f->name);
        t_putchar('\n');
    }
    return 0;
}

int tmpfs_complete(int ns, const char *dir, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx)
{
    (void)dir;   /* flat namespace; dir is always the mount root */
    if (!cb) return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (tmpfile_t *f = s_files; f; f = f->next) {
        if (f->ns != ns) continue;
        if (plen == 0 || strncmp(f->name, prefix, plen) == 0)
            cb(f->name, 0, ctx);
    }
    return 0;
}

long tmpfs_size(int ns, const char *path)
{
    const char *name = leaf_name(path);
    tmpfile_t  *f    = name ? find(ns, name) : NULL;
    if (!f) return -1;
    return (long)f->len;
}

int tmpfs_mkdir(int ns, const char *path)
{
    /* Flat namespace -- directories are conceptual.  Accept any non-root
     * path so scripts can `mkdir /tmp/sub` before writing files into it
     * without seeing a spurious failure. */
    (void)ns;
    if (!path || path[0] != '/' || path[1] == '\0') return -1;
    return 0;
}

int tmpfs_delete(int ns, const char *path)
{
    const char *name = leaf_name(path);
    if (!name) return -1;
    tmpfile_t **link = &s_files;
    while (*link && !((*link)->ns == ns && strcmp((*link)->name, name) == 0))
        link = &(*link)->next;
    if (!*link) return -1;
    tmpfile_t *f = *link;
    *link = f->next;
    kfree(f->buf);
    kfree(f);
    return 0;
}
