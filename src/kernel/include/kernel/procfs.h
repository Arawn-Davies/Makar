#ifndef _KERNEL_PROCFS_H
#define _KERNEL_PROCFS_H

/*
 * procfs - synthetic /proc filesystem.
 *
 * Linux-style read-only view of kernel state.  Each entry is generated
 * on demand by a renderer that writes ASCII into the caller's buffer:
 *
 *   /proc/cpuinfo  CPUID vendor / family / model / features
 *   /proc/meminfo  PMM frame count + heap usage
 *   /proc/tasks    one row per live task (pid name state tty cwd)
 *   /proc/uname    kernel name, version, build timestamp
 *
 * No write support.  VFS routes paths under /proc here.
 */

#include <stdint.h>
#include <stddef.h>

#include <kernel/fat32.h>   /* fat32_complete_cb_t */

/* Mount-point prefix.  Single source of truth: procfs error messages
 * and any other consumer that needs to format absolute /proc paths
 * should derive them from this string rather than hardcoding "/proc". */
#define PROCFS_MOUNT      "/proc"
#define PROCFS_MOUNT_LEN  5

/* List the /proc directory (or report 'not a dir' for entry paths) to
 * the current terminal.  Returns 0. */
int procfs_ls(const char *path);

/* Read entire synthesised content of /proc/<entry> into buf (up to
 * bufsz bytes).  Returns 0 on success, -1 if path is unknown.
 * *out_sz receives the number of bytes written. */
int procfs_read_file(const char *path, void *buf, uint32_t bufsz,
                     uint32_t *out_sz);

/* Tab-completion enumeration: invokes cb for each entry under /proc
 * whose name starts with prefix.  Returns 0. */
int procfs_complete(const char *dir, const char *prefix,
                    fat32_complete_cb_t cb, void *ctx);

/* Returns 1 if path names a known /proc entry, 0 otherwise. */
int procfs_file_exists(const char *path);

#endif /* _KERNEL_PROCFS_H */
