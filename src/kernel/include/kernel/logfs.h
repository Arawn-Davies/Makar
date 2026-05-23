#ifndef _KERNEL_LOGFS_H
#define _KERNEL_LOGFS_H

/*
 * logfs.h - synthetic, writable /log directory.
 *
 * /log is an in-RAM, dmesg-style log tree, independent of any disk
 * filesystem.  Unlike /proc and /dev (which synthesise their contents on
 * read), /log is a real append store: the kernel and userspace programs
 * write discrete log files into it that survive even when the operation
 * doing the logging is failing on the very volume it would otherwise write.
 *
 *   /log/kernel.log   - the serial debug stream (tee'd from Serial_WriteString)
 *   /log/install.log  - the installer's per-step progress log
 *   /log/<name>       - created on first write by any program
 *
 * kernel.log is backed by a static buffer so the serial tee is safe during
 * very early boot (before the heap exists).  Every other file is allocated
 * lazily from the heap on first write.  Each file is a fixed-capacity ring:
 * once full, the oldest half is dropped so the newest (most relevant) output
 * is always retained.
 *
 * Paths passed to these functions are logfs-relative and always start with
 * '/': "/" is the directory itself, "/<name>" a specific file.  /log is flat
 * (no sub-directories).
 */

#include <kernel/types.h>
#include <kernel/fat32.h>   /* fat32_complete_cb_t */

#define LOGFS_MOUNT      "/log"
#define LOGFS_MOUNT_LEN  4

/* Read a log file into a caller buffer.  Returns 0 and writes the byte
 * count into *out_sz, or -1 if the file does not exist. */
long logfs_read(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz);

/* Append bytes to a log file, creating it on first write.  Returns the byte
 * count appended, or -1 (bad path / table full / out of memory). */
long logfs_write(const char *path, const void *buf, uint32_t len);

/* 1 if "/<name>" is an existing log file, else 0. */
int  logfs_file_exists(const char *path);

/* List the /log directory (path "/") to the terminal. */
int  logfs_ls(const char *path);

/* Tab-completion enumeration of /log entries. */
int  logfs_complete(const char *dir, const char *prefix,
                    fat32_complete_cb_t cb, void *ctx);

/* --- kernel-facing convenience wrappers (operate on a named file) --- */

/* Append raw bytes to kernel.log (the serial tee).  Pre-heap safe. */
void logfs_kwrite(const char *s, uint32_t n);

/* Clear kernel.log. */
void logfs_kreset(void);

/* Append one line (a trailing newline is added) to the file <name>
 * (no leading slash, e.g. "install.log"). */
void logfs_append_line(const char *name, const char *line);

#endif /* _KERNEL_LOGFS_H */
