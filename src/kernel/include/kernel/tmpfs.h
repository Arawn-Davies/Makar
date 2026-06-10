#ifndef _KERNEL_TMPFS_H
#define _KERNEL_TMPFS_H

/*
 * tmpfs.h - in-RAM writable /tmp directory.
 *
 * /tmp is a scratch ramdisk for programs that need a writable filesystem
 * without first having to mkfs+mount a real disk.  Cousins of logfs:
 * same flat namespace, heap-backed file records created on first write,
 * but with OVERWRITE semantics instead of logfs's append-only ring.
 * There is no fixed file-table limit; available kernel heap is the real
 * limit, with a per-file guard to keep one write from consuming all RAM.
 * That makes it suitable as a tcc(1) output sink: a successive
 * sys_open(O_TRUNC) + sys_write loop replaces the file rather than
 * tacking on the end of the previous run.
 *
 * Paths passed in are tmpfs-relative and always start with '/':
 * "/" is the directory itself, "/<name>" a specific file.  Sub-paths
 * like "/foo/bar.o" are accepted: tmpfs stores the entire path past
 * the leading '/' as a single flat name, so `/tmp/foo/bar.o` and
 * `/tmp/foo_bar.o` are two distinct files but `/tmp/foo` does not
 * exist as a true directory.  `tmpfs_mkdir` is a no-op success that
 * exists so scripts which `mkdir /tmp/sub` before writing files into
 * it don't see a spurious failure.
 */

#include <kernel/types.h>
#include <kernel/fat32.h>   /* fat32_complete_cb_t */

#define TMPFS_MOUNT      "/tmp"
#define TMPFS_MOUNT_LEN  4

/* Every op takes a namespace id `ns` so several independent tmpfs instances
 * can be mounted at once -- the canonical /tmp is ns 0, and a read-only live
 * root adds writable home overlays (/root, /home/user) as separate namespaces.
 * Entries in different namespaces never alias even when their relative paths
 * match. */

/* Read a tmpfs file into a caller buffer.  Returns 0 on success and
 * stores the byte count in *out_sz; returns -1 if the file does not
 * exist. */
long tmpfs_read(int ns, const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz);

/* Replace the contents of a tmpfs file (creating it on first write).
 * Returns the byte count written, or -1 on bad path / oversize write /
 * OOM.  Unlike logfs_write this overwrites - there is no ring. */
long tmpfs_write(int ns, const char *path, const void *buf, uint32_t len);

/* 1 if "/<name>" is an existing tmpfs file, else 0. */
int  tmpfs_file_exists(int ns, const char *path);

/* List the tmpfs directory (path "/") to the terminal. */
int  tmpfs_ls(int ns, const char *path);

/* Tab-completion enumeration of tmpfs entries. */
int  tmpfs_complete(int ns, const char *dir, const char *prefix,
                    fat32_complete_cb_t cb, void *ctx);

/* Return the current size of "/<name>", or -1 if absent. */
long tmpfs_size(int ns, const char *path);

/* Remove "/<name>".  Returns 0 on success, -1 if absent. */
int  tmpfs_delete(int ns, const char *path);

/* No-op success: tmpfs has a flat namespace, so "creating" a directory
 * is implicit -- the namespace it spans appears the moment a file
 * underneath it is written.  Returns 0 for any non-empty path so
 * scripts that mkdir before writing don't see a spurious error. */
int  tmpfs_mkdir(int ns, const char *path);

#endif /* _KERNEL_TMPFS_H */
