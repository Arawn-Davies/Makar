/*
 * logfs.c - synthetic, writable /log directory.  See kernel/logfs.h.
 *
 * A flat table of in-RAM log files.  kernel.log is statically allocated so
 * the serial tee works before the heap is up; every other file is created
 * lazily from the heap on first write.  Each file is a fixed-capacity ring:
 * when it fills, the oldest half is discarded so the tail (where a failure
 * lives) is always preserved.
 */

#include <kernel/logfs.h>
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <string.h>
#include <stddef.h>

#define LOGFS_MAX_FILES   16
#define LOGFS_NAME_MAX    32
#define LOGFS_KERNEL_CAP  (64u * 1024u)   /* static; serial tee, pre-heap safe */
#define LOGFS_FILE_CAP    (32u * 1024u)   /* heap; per dynamically-created file */

typedef struct {
    char     name[LOGFS_NAME_MAX];
    char    *buf;
    uint32_t cap;
    uint32_t len;
    int      in_use;
} logfile_t;

/* kernel.log lives in BSS so Serial_WriteString can tee into it during very
 * early boot, long before heap_init().  The s_files[0] entry below points at
 * it via a static initialiser (the address of a static array is a constant). */
static char s_kernel_buf[LOGFS_KERNEL_CAP];

static logfile_t s_files[LOGFS_MAX_FILES] = {
    [0] = { .name = "kernel.log", .buf = s_kernel_buf,
            .cap = LOGFS_KERNEL_CAP, .len = 0, .in_use = 1 },
};

/* -------------------------------------------------------------------------
 * Ring append - keep the newest output once the buffer fills (dmesg-style).
 * ---------------------------------------------------------------------- */
static void ring_append(logfile_t *f, const char *s, uint32_t n)
{
    if (!s || n == 0 || !f->buf || f->cap == 0) return;

    if (n >= f->cap) {                    /* single write larger than buffer */
        s += (n - (f->cap - 1u));
        n  = f->cap - 1u;
    }
    if (f->len + n + 1u >= f->cap) {
        uint32_t keep = f->cap / 2u;
        if (f->len > keep) {
            memmove(f->buf, f->buf + (f->len - keep), keep);
            f->len = keep;
        } else {
            f->len = 0;
        }
    }
    memcpy(f->buf + f->len, s, n);
    f->len += n;
    f->buf[f->len] = '\0';
}

/* Strip the leading '/' from a logfs-relative path, rejecting deeper paths
 * (/log is flat).  Returns the bare name, or NULL for "/" / malformed. */
static const char *leaf_name(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0') return NULL;
    const char *name = path + 1;
    for (const char *q = name; *q; q++)
        if (*q == '/') return NULL;       /* deeper than one level */
    return name;
}

/* Find an existing file by bare name. */
static logfile_t *find(const char *name)
{
    for (int i = 0; i < LOGFS_MAX_FILES; i++)
        if (s_files[i].in_use && strcmp(s_files[i].name, name) == 0)
            return &s_files[i];
    return NULL;
}

/* Find or lazily create a heap-backed file by bare name.  NULL if the name
 * is too long, the table is full, or the heap allocation fails. */
static logfile_t *find_or_create(const char *name)
{
    logfile_t *f = find(name);
    if (f) return f;
    if (strlen(name) >= LOGFS_NAME_MAX) return NULL;

    for (int i = 0; i < LOGFS_MAX_FILES; i++) {
        if (s_files[i].in_use) continue;
        char *buf = (char *)kmalloc(LOGFS_FILE_CAP);
        if (!buf) return NULL;
        buf[0] = '\0';
        strncpy(s_files[i].name, name, LOGFS_NAME_MAX - 1);
        s_files[i].name[LOGFS_NAME_MAX - 1] = '\0';
        s_files[i].buf    = buf;
        s_files[i].cap    = LOGFS_FILE_CAP;
        s_files[i].len    = 0;
        s_files[i].in_use = 1;
        return &s_files[i];
    }
    return NULL;   /* table full */
}

/* -------------------------------------------------------------------------
 * Public VFS-facing API (paths are logfs-relative, leading '/').
 * ---------------------------------------------------------------------- */

long logfs_read(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz)
{
    const char *name = leaf_name(path);
    logfile_t  *f    = name ? find(name) : NULL;
    if (!f) { if (out_sz) *out_sz = 0; return -1; }

    uint32_t n = (f->len < bufsz) ? f->len : bufsz;
    memcpy(buf, f->buf, n);
    if (out_sz) *out_sz = n;
    return 0;
}

long logfs_write(const char *path, const void *buf, uint32_t len)
{
    const char *name = leaf_name(path);
    if (!name) return -1;
    logfile_t *f = find_or_create(name);
    if (!f) return -1;
    ring_append(f, (const char *)buf, len);
    return (long)len;
}

int logfs_file_exists(const char *path)
{
    const char *name = leaf_name(path);
    return (name && find(name)) ? 1 : 0;
}

int logfs_ls(const char *path)
{
    /* /log is flat: only "/" lists; "/<name>" is a file, not a directory. */
    if (path && path[0] == '/' && path[1] != '\0') {
        t_writestring("ls: " LOGFS_MOUNT);
        t_writestring(path);
        t_writestring(logfs_file_exists(path) ? ": Not a directory\n"
                                              : ": No such entry\n");
        return -1;
    }
    for (int i = 0; i < LOGFS_MAX_FILES; i++) {
        if (!s_files[i].in_use) continue;
        t_writestring(s_files[i].name);
        t_putchar('\n');
    }
    return 0;
}

int logfs_complete(const char *dir, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx)
{
    (void)dir;   /* /log is flat; dir is always "/log" */
    if (!cb) return -1;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < LOGFS_MAX_FILES; i++) {
        if (!s_files[i].in_use) continue;
        if (plen == 0 || strncmp(s_files[i].name, prefix, plen) == 0)
            cb(s_files[i].name, 0, ctx);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Kernel-facing convenience wrappers.
 * ---------------------------------------------------------------------- */

void logfs_kwrite(const char *s, uint32_t n)
{
    ring_append(&s_files[0], s, n);    /* s_files[0] is kernel.log (static) */
}

void logfs_kreset(void)
{
    s_files[0].len = 0;
    s_files[0].buf[0] = '\0';
}

void logfs_append_line(const char *name, const char *line)
{
    if (!name || !line) return;
    logfile_t *f = find_or_create(name);
    if (!f) return;
    uint32_t n = 0;
    while (line[n]) n++;
    ring_append(f, line, n);
    ring_append(f, "\n", 1);
}
