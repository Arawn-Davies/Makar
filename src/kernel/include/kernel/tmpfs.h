#ifndef _KERNEL_TMPFS_H
#define _KERNEL_TMPFS_H

/*
 * tmpfs.h - in-RAM writable /tmp directory.
 *
 * /tmp is a scratch ramdisk for programs that need a writable filesystem
 * without first having to mkfs+mount a real disk.  Cousins of logfs:
 * same flat per-name table, same lazy heap allocation on first write,
 * but with OVERWRITE semantics instead of logfs's append-only ring.
 * That makes it suitable as a tcc(1) output sink: a successive
 * sys_open(O_TRUNC) + sys_write loop replaces the file rather than
 * tacking on the end of the previous run.
 *
 * Paths passed in are tmpfs-relative and always start with '/':
 * "/" is the directory itself, "/<name>" a specific file.  /tmp is
 * flat (no sub-directories).
 */

#include <kernel/types.h>
#include <kernel/fat32.h>   /* fat32_complete_cb_t */

#define TMPFS_MOUNT      "/tmp"
#define TMPFS_MOUNT_LEN  4

/* Read a tmpfs file into a caller buffer.  Returns 0 on success and
 * stores the byte count in *out_sz; returns -1 if the file does not
 * exist. */
long tmpfs_read(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz);

/* Replace the contents of a tmpfs file (creating it on first write).
 * Returns the byte count written, or -1 on bad path / table full /
 * OOM.  Unlike logfs_write this overwrites - there is no ring. */
long tmpfs_write(const char *path, const void *buf, uint32_t len);

/* 1 if "/<name>" is an existing tmpfs file, else 0. */
int  tmpfs_file_exists(const char *path);

/* List the /tmp directory (path "/") to the terminal. */
int  tmpfs_ls(const char *path);

/* Tab-completion enumeration of /tmp entries. */
int  tmpfs_complete(const char *dir, const char *prefix,
                    fat32_complete_cb_t cb, void *ctx);

/* Return the current size of "/<name>", or -1 if absent. */
long tmpfs_size(const char *path);

/* Remove "/<name>".  Returns 0 on success, -1 if absent. */
int  tmpfs_delete(const char *path);

#endif /* _KERNEL_TMPFS_H */
