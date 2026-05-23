/*
 * tmpfs.c - synthetic, writable /tmp directory.  See kernel/tmpfs.h.
 *
 * Flat table of in-RAM scratch files.  Each file is lazily allocated
 * from the kernel heap on first write; later writes overwrite the
 * buffer wholesale (no ring, unlike logfs).  The intended consumer is
 * tcc(1) producing an ELF in /tmp before the shell `exec`s it.
 */

#include <kernel/tmpfs.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <string.h>
#include <stddef.h>

#define TMPFS_MAX_FILES   16
#define TMPFS_NAME_MAX    64
#define TMPFS_FILE_CAP    (512u * 1024u)   /* 512 KiB per file -- ample for hello.elf */

typedef struct {
    char     name[TMPFS_NAME_MAX];
    char    *buf;
    uint32_t cap;
    uint32_t len;
    int      in_use;
} tmpfile_t;

static tmpfile_t s_files[TMPFS_MAX_FILES];

/* Strip the leading '/' from a tmpfs-relative path, rejecting deeper paths
 * (/tmp is flat).  Returns the bare name, or NULL for "/" / malformed. */
static const char *leaf_name(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0') return NULL;
    const char *name = path + 1;
    for (const char *q = name; *q; q++)
        if (*q == '/') return NULL;       /* deeper than one level */
    return name;
}

/* Find an existing file by bare name. */
static tmpfile_t *find(const char *name)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        if (s_files[i].in_use && strcmp(s_files[i].name, name) == 0)
            return &s_files[i];
    return NULL;
}

/* Find or lazily create a heap-backed file by bare name.  Returns NULL
 * if the name is too long, the table is full, or the heap allocation
 * fails. */
static tmpfile_t *find_or_create(const char *name)
{
    tmpfile_t *f = find(name);
    if (f) return f;
    if (strlen(name) >= TMPFS_NAME_MAX) return NULL;

    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (s_files[i].in_use) continue;
        char *buf = (char *)kmalloc(TMPFS_FILE_CAP);
        if (!buf) return NULL;
        strncpy(s_files[i].name, name, TMPFS_NAME_MAX - 1);
        s_files[i].name[TMPFS_NAME_MAX - 1] = '\0';
        s_files[i].buf    = buf;
        s_files[i].cap    = TMPFS_FILE_CAP;
        s_files[i].len    = 0;
        s_files[i].in_use = 1;
        return &s_files[i];
    }
    return NULL;   /* table full */
}

/* -------------------------------------------------------------------------
 * Public VFS-facing API (paths are tmpfs-relative, leading '/').
 * ---------------------------------------------------------------------- */

long tmpfs_read(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    const char *name = leaf_name(path);
    tmpfile_t  *f    = name ? find(name) : NULL;
    if (!f) { if (out_sz) *out_sz = 0; return -1; }

    uint32_t n = (f->len < bufsz) ? f->len : bufsz;
    if (buf && n > 0)
        memcpy(buf, f->buf, n);
    if (out_sz) *out_sz = n;
    return 0;
}

long tmpfs_write(const char *path, const void *buf, uint32_t len)
{
    const char *name = leaf_name(path);
    if (!name) return -1;
    tmpfile_t *f = find_or_create(name);
    if (!f) return -1;
    if (len > f->cap) return -1;       /* refuse oversized write */
    if (len > 0 && buf)
        memcpy(f->buf, buf, len);
    f->len = len;
    return (long)len;
}

int tmpfs_file_exists(const char *path)
{
    const char *name = leaf_name(path);
    return (name && find(name)) ? 1 : 0;
}

int tmpfs_ls(const char *path)
{
    /* /tmp is flat: only "/" lists; "/<name>" is a file, not a directory. */
    if (path && path[0] == '/' && path[1] != '\0') {
        t_writestring("ls: " TMPFS_MOUNT);
        t_writestring(path);
        t_writestring(tmpfs_file_exists(path) ? ": Not a directory\n"
                                              : ": No such entry\n");
        return -1;
    }
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!s_files[i].in_use) continue;
        t_writestring(s_files[i].name);
        t_putchar('\n');
    }
    return 0;
}

int tmpfs_complete(const char *dir, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx)
{
    (void)dir;   /* /tmp is flat; dir is always "/tmp" */
    if (!cb) return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!s_files[i].in_use) continue;
        if (plen == 0 || strncmp(s_files[i].name, prefix, plen) == 0)
            cb(s_files[i].name, 0, ctx);
    }
    return 0;
}

long tmpfs_size(const char *path)
{
    const char *name = leaf_name(path);
    tmpfile_t  *f    = name ? find(name) : NULL;
    if (!f) return -1;
    return (long)f->len;
}

int tmpfs_delete(const char *path)
{
    const char *name = leaf_name(path);
    tmpfile_t  *f    = name ? find(name) : NULL;
    if (!f) return -1;
    kfree(f->buf);
    f->buf    = NULL;
    f->cap    = 0;
    f->len    = 0;
    f->name[0] = '\0';
    f->in_use = 0;
    return 0;
}
