#ifndef _KERNEL_PAGECACHE_H
#define _KERNEL_PAGECACHE_H

#include <stdint.h>

/*
 * File page cache, shared between read() and mmap().
 *
 * Sits under SYS_READ for lazily-opened read-only files: instead of eager-
 * loading a whole file into a kmalloc'd buffer at open(), the file is streamed
 * a page at a time through this cache (like Linux's page cache).  Misses are
 * filled via vfs_read_at(); hits avoid the disk entirely.
 *
 * Each valid slot owns a page-aligned, refcounted PMM frame, bounded to
 * PC_NPAGES (1 MiB).  Because the data lives in a real frame, the same physical
 * page backs both read() and a file mmap(): pagecache_acquire() pins a frame
 * and the mmap path maps it read-only into user space, so every process that
 * maps the same file (libc.so above all) shares one copy in RAM.
 *
 * Correctness: caches clean read-only data only.  Any mutation of a path must
 * call pagecache_invalidate(path) (wired into the VFS write/delete/rename
 * paths) so a subsequent read never sees stale bytes.
 */

#define PAGECACHE_PGSZ 4096u

/* Serve [off, off+len) of `path` (whose current size is `size`) into `buf`,
 * filling misses from disk via vfs_read_at().  Returns bytes read (>=0, 0 at or
 * past EOF) or -1 on error / non-cacheable backend. */
long pagecache_read(const char *path, uint32_t size,
                    uint32_t off, void *buf, uint32_t len);

/* Pin file page `pidx` of `path` (current size `size`) and return the phys
 * address of its backing frame, with the frame's refcount bumped for the
 * caller (balance it with pmm_free_frame when the mapping is torn down).  The
 * frame's tail past EOF is zero-filled, so the whole 4 KiB is safe to map.
 * `*out_len` (if non-NULL) gets the valid byte count.  Returns 0 on EOF/error.
 * This is the mmap counterpart to pagecache_read: both share the cache frame. */
uint32_t pagecache_acquire(const char *path, uint32_t size,
                           uint32_t pidx, uint32_t *out_len);

/* Drop every cached page belonging to `path` (call on write/truncate/delete/
 * rename so a later read re-fetches fresh data). */
void pagecache_invalidate(const char *path);

/* Drop the entire cache (e.g. on unmount). */
void pagecache_drop_all(void);

#endif /* _KERNEL_PAGECACHE_H */
