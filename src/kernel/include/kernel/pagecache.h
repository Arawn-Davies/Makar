#ifndef _KERNEL_PAGECACHE_H
#define _KERNEL_PAGECACHE_H

#include <stdint.h>

/*
 * Read-only file page cache.
 *
 * Sits under SYS_READ for lazily-opened read-only files: instead of eager-
 * loading a whole file into a kmalloc'd buffer at open(), the file is streamed
 * a page at a time through this cache (WWLD -- Linux's page cache).  Misses are
 * filled via vfs_read_at(); hits avoid the disk entirely.
 *
 * Backing store is a fixed static pool (bounded footprint, self-evicting LRU),
 * so it never competes with the PMM for frames and there is no reclaim lock-
 * ordering hazard under preemptive syscalls.  (A dynamic, PMM-backed cache with
 * a pressure-driven shrinker is a documented follow-up.)
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

/* Drop every cached page belonging to `path` (call on write/truncate/delete/
 * rename so a later read re-fetches fresh data). */
void pagecache_invalidate(const char *path);

/* Drop the entire cache (e.g. on unmount). */
void pagecache_drop_all(void);

#endif /* _KERNEL_PAGECACHE_H */
