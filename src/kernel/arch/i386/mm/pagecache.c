/*
 * pagecache.c -- file page cache, shared between read() and mmap() (see
 * pagecache.h).
 *
 * Fixed pool of PC_NPAGES slots in a doubly-linked LRU list (MRU at head, LRU
 * at tail).  Lookup is a linear scan keyed by {path-hash, page index} with a
 * full-path tie-break; a miss reuses the LRU tail and fills it via vfs_read_at.
 *
 * Unlike the original inline-buffer cache, each valid slot now owns a
 * page-aligned, refcounted PMM frame (`frame`) rather than an inline `data[]`
 * array.  That is what lets the same physical page back both `read()` (memcpy
 * out of the frame) and a file `mmap()` (map the frame straight into user space
 * read-only): pagecache_acquire() bumps the frame's refcount and hands the
 * phys address to the mmap path, so N processes mapping libc.so share one copy.
 * The cache keeps its own ref on every valid frame; eviction/invalidation drops
 * it, and the PMM frees the frame only once the cache and every mapper let go.
 * This is the Linux page-cache model on a small scale.
 *
 * An irq lock keeps the list consistent once syscalls run preemptively.  Lock
 * order: pagecache lock is taken alone or *before* the VFS disk big-lock
 * (pagecache_read -> vfs_read_at).  Invalidation is invoked from the VFS mutate
 * paths *outside* that lock, so there is no reverse ordering.
 */
#include <kernel/pagecache.h>
#include <kernel/pmm.h>
#include <kernel/vfs.h>
#include <string.h>

#define PC_PGSZ   PAGECACHE_PGSZ
#define PC_NPAGES 256u                 /* up to 256 * 4 KiB = 1 MiB of frames */

typedef struct pcpage {
    int            valid;
    uint32_t       hash;               /* hash of path (fast reject)        */
    char           path[VFS_PATH_MAX];
    uint32_t       pidx;               /* page index within the file        */
    uint32_t       len;                /* valid bytes in the frame (< PGSZ at EOF) */
    uint32_t       frame;              /* phys addr of the backing frame (0 = none) */
    struct pcpage *lp, *ln;            /* LRU links (prev=toward MRU)       */
} pcpage_t;

static pcpage_t  s_pages[PC_NPAGES];
static pcpage_t *s_mru, *s_lru;        /* list ends                         */
static int       s_inited;

static inline uint32_t pc_irq_save(void)
{ uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void pc_irq_restore(uint32_t f)
{ __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }

static uint32_t pc_hash(const char *s)
{
    uint32_t h = 2166136261u;          /* FNV-1a */
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static int pc_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pc_init(void)
{
    for (uint32_t i = 0; i < PC_NPAGES; i++) {
        s_pages[i].valid = 0;
        s_pages[i].frame = 0;
        s_pages[i].lp = (i == 0) ? 0 : &s_pages[i - 1];
        s_pages[i].ln = (i == PC_NPAGES - 1) ? 0 : &s_pages[i + 1];
    }
    s_mru = &s_pages[0];
    s_lru = &s_pages[PC_NPAGES - 1];
    s_inited = 1;
}

/* Unlink p from the LRU list. */
static void pc_unlink(pcpage_t *p)
{
    if (p->lp) p->lp->ln = p->ln; else s_mru = p->ln;
    if (p->ln) p->ln->lp = p->lp; else s_lru = p->lp;
    p->lp = p->ln = 0;
}

/* Insert p at the MRU head. */
static void pc_push_front(pcpage_t *p)
{
    p->lp = 0; p->ln = s_mru;
    if (s_mru) s_mru->lp = p;
    s_mru = p;
    if (!s_lru) s_lru = p;
}

/* Insert p at the LRU tail (so it is the next slot recycled). */
static void pc_push_back(pcpage_t *p)
{
    p->ln = 0; p->lp = s_lru;
    if (s_lru) s_lru->ln = p;
    s_lru = p;
    if (!s_mru) s_mru = p;
}

static void pc_touch(pcpage_t *p)
{
    if (s_mru == p) return;
    pc_unlink(p);
    pc_push_front(p);
}

/* Caller holds the pc lock.  Return the page for (path,pidx), filling a miss
 * from disk via vfs_read_at into a refcounted frame on the recycled LRU tail.
 * NULL on fill / allocation error. */
static pcpage_t *pc_get(const char *path, uint32_t hash, uint32_t pidx)
{
    for (pcpage_t *p = s_mru; p; p = p->ln) {
        if (p->valid && p->pidx == pidx && p->hash == hash &&
            pc_streq(p->path, path)) {
            pc_touch(p);
            return p;
        }
    }

    /* Miss: recycle the LRU tail. */
    pcpage_t *p = s_lru;
    if (!p) return 0;

    /* Pick a frame to fill.  Reuse the slot's own frame iff nothing else
     * references it (refcount 1 == just the cache); otherwise a mapper still
     * holds the previous page's content, so drop the cache's ref (the mapper
     * keeps it alive) and take a fresh frame. */
    uint32_t frame = p->frame;
    if (frame && pmm_ref_count(frame) != 1) {
        pmm_free_frame(frame);
        frame = 0;
    }
    if (!frame) {
        frame = pmm_alloc_frame();     /* refcount 1: the cache's own ref */
        if (!frame) return 0;          /* OOM: leave the slot untouched */
    }

    long got = vfs_read_at(path, pidx * PC_PGSZ, (void *)frame, PC_PGSZ);
    if (got < 0) { p->frame = frame; p->valid = 0; return 0; }

    /* Zero the tail past EOF so a whole-frame mmap sees defined bytes. */
    if ((uint32_t)got < PC_PGSZ)
        memset((uint8_t *)frame + got, 0, PC_PGSZ - (uint32_t)got);

    p->frame = frame;
    p->valid = 1;
    p->hash  = hash;
    p->pidx  = pidx;
    p->len   = (uint32_t)got;
    /* Copy the path (bounded by VFS_PATH_MAX). */
    uint32_t i = 0;
    while (path[i] && i < VFS_PATH_MAX - 1) { p->path[i] = path[i]; i++; }
    p->path[i] = '\0';
    pc_touch(p);
    return p;
}

long pagecache_read(const char *path, uint32_t size,
                    uint32_t off, void *buf, uint32_t len)
{
    if (off >= size) return 0;
    uint32_t avail   = size - off;
    uint32_t to_read = (len < avail) ? len : avail;
    uint8_t *out     = (uint8_t *)buf;
    uint32_t hash    = pc_hash(path);
    uint32_t done    = 0;

    uint32_t fl = pc_irq_save();
    if (!s_inited) pc_init();

    while (done < to_read) {
        uint32_t cur   = off + done;
        uint32_t pidx  = cur / PC_PGSZ;
        uint32_t poff  = cur % PC_PGSZ;
        pcpage_t *p = pc_get(path, hash, pidx);
        if (!p) { pc_irq_restore(fl); return done ? (long)done : -1; }
        if (poff >= p->len) break;                  /* short page = EOF */
        uint32_t chunk = p->len - poff;
        if (chunk > to_read - done) chunk = to_read - done;
        memcpy(out + done, (uint8_t *)p->frame + poff, chunk);
        done += chunk;
        if (p->len < PC_PGSZ) break;                /* last (partial) page */
    }
    pc_irq_restore(fl);
    return (long)done;
}

uint32_t pagecache_acquire(const char *path, uint32_t size,
                           uint32_t pidx, uint32_t *out_len)
{
    if ((uint64_t)pidx * PC_PGSZ >= size) return 0;     /* page past EOF */
    uint32_t hash = pc_hash(path);

    uint32_t fl = pc_irq_save();
    if (!s_inited) pc_init();
    pcpage_t *p = pc_get(path, hash, pidx);
    if (!p || !p->frame) { pc_irq_restore(fl); return 0; }

    /* Hand the caller a ref on the frame; it owns it until the mapping is torn
     * down (pmm_free_frame in vmm_unmap_and_free / vmm_free_pd). */
    pmm_inc_ref(p->frame);
    uint32_t frame = p->frame;
    if (out_len) *out_len = p->len;
    pc_irq_restore(fl);
    return frame;
}

void pagecache_invalidate(const char *path)
{
    uint32_t hash = pc_hash(path);
    uint32_t fl = pc_irq_save();
    if (s_inited) {
        for (uint32_t i = 0; i < PC_NPAGES; i++) {
            pcpage_t *p = &s_pages[i];
            if (p->valid && p->hash == hash && pc_streq(p->path, path)) {
                p->valid = 0;
                if (p->frame) { pmm_free_frame(p->frame); p->frame = 0; }
                pc_unlink(p);
                pc_push_back(p);    /* invalidated slot -> recycle it first */
            }
        }
    }
    pc_irq_restore(fl);
}

void pagecache_drop_all(void)
{
    uint32_t fl = pc_irq_save();
    for (uint32_t i = 0; i < PC_NPAGES; i++) {
        if (s_pages[i].frame) { pmm_free_frame(s_pages[i].frame); s_pages[i].frame = 0; }
        s_pages[i].valid = 0;
    }
    s_inited = 0;                  /* re-init the LRU list on next use */
    pc_irq_restore(fl);
}
