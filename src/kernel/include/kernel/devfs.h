#ifndef _KERNEL_DEVFS_H
#define _KERNEL_DEVFS_H

/*
 * devfs - synthetic /dev filesystem exposing raw block devices.
 *
 * Unlike procfs (which renders text on demand), devfs nodes are windows
 * onto IDE storage addressed by byte offset:
 *
 *   /dev/hda        whole ATA disk 0          (read/write)
 *   /dev/hda1..4    its MBR/GPT partitions    (read/write)
 *   /dev/hdb ...    further ATA disks
 *   /dev/cdrom      the ATAPI CD-ROM          (read-only)
 *
 * Reads/writes are sector-granular under the hood (512 B for ATA, 2048 B
 * for ATAPI) but devfs presents a flat byte-addressed view with internal
 * read-modify-write so callers can transfer arbitrary offsets/lengths.
 *
 * The fd layer opens a /dev node as FD_KIND_BLOCKDEV (no eager buffer);
 * SYS_READ/SYS_WRITE/SYS_LSEEK route through devfs_pread / devfs_pwrite.
 */

#include <stdint.h>
#include <stddef.h>

#include <kernel/fat32.h>   /* fat32_complete_cb_t */

#define DEVFS_MOUNT      "/dev"
#define DEVFS_MOUNT_LEN  4

/* Re-scan the IDE bus and rebuild the node table.  Call after ide_init
 * (and again after fdisk rewrites a partition table).  Idempotent. */
void devfs_init(void);

/* Returns 1 if path names a known /dev node, 0 otherwise.
 * path is devfs-relative and starts with '/' (e.g. "/hda1"). */
int devfs_file_exists(const char *path);

/* Resolve a devfs-relative path to a node index (>= 0), or -1 if unknown. */
int devfs_lookup(const char *path);

/* Total addressable byte size of node `idx` (0 if idx is invalid). */
uint32_t devfs_node_size(int idx);

/* 1 if node `idx` is read-only (e.g. CD-ROM), else 0. */
int devfs_node_readonly(int idx);

/* Byte-addressed read/write against node `idx`.  Handle arbitrary
 * offset/length via internal sector RMW.  Return bytes transferred
 * (clamped to the node size), or -1 on error. */
long devfs_pread(int idx, void *buf, uint32_t len, uint32_t off);
long devfs_pwrite(int idx, const void *buf, uint32_t len, uint32_t off);

/* List /dev (or report 'not a directory' for a node path) to the
 * current terminal.  Returns 0. */
int devfs_ls(const char *path);

/* Tab-completion enumeration: invoke cb for each node whose name starts
 * with prefix.  Returns 0. */
int devfs_complete(const char *dir, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx);

#endif /* _KERNEL_DEVFS_H */
