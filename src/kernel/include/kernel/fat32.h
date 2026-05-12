#ifndef _KERNEL_FAT32_H
#define _KERNEL_FAT32_H

/*
 * fat32.h - FAT32 filesystem driver public API.
 *
 * Supports one mounted volume at a time.  All paths accept both '\' and '/'
 * as separators and are matched case-insensitively.  Absolute paths start
 * with '\' or '/'; relative paths are resolved against the current working
 * directory.
 *
 * Read support: directory listing (8.3 + LFN), file read, cd.
 * Write support: file create/overwrite, directory creation, FAT32 format.
 *
 * All sector I/O is delegated to ide_read_sectors() / ide_write_sectors().
 */

#include <kernel/types.h>

/* -------------------------------------------------------------------------
 * Mount / unmount
 * ---------------------------------------------------------------------- */

/*
 * fat32_mount – mount a FAT32 partition.
 *
 * drive    : IDE drive number (0–3)
 * part_lba : LBA of the first sector of the partition (BPB sector)
 *
 * Returns 0 on success, or a negative error code:
 *   -1  I/O error
 *   -2  not a valid FAT32 volume (bad signature or BPB fields)
 */
int fat32_mount(uint8_t drive, uint32_t part_lba);

/* Unmount the current volume. */
void fat32_unmount(void);

/* Returns 1 if a volume is currently mounted, 0 otherwise. */
int fat32_mounted(void);

/* -------------------------------------------------------------------------
 * Directory operations
 * ---------------------------------------------------------------------- */

/*
 * fat32_ls – list directory contents to the terminal.
 *
 * path : absolute or relative path to a directory.
 *        NULL or empty string → current working directory.
 *
 * Returns 0 on success, negative on error.
 */
int fat32_ls(const char *path);

/*
 * fat32_cd – change the current working directory.
 *
 * Returns 0 on success, negative on error.
 */
int fat32_cd(const char *path);

/*
 * fat32_getcwd – return a pointer to the current working directory string.
 * The returned string is owned by the driver; do not free or modify it.
 */
const char *fat32_getcwd(void);

/*
 * fat32_mkdir – create a directory (the parent directory must exist).
 *
 * Returns 0 on success, negative on error.
 */
int fat32_mkdir(const char *path);

/* -------------------------------------------------------------------------
 * File I/O
 * ---------------------------------------------------------------------- */

/*
 * fat32_read_file – read a file into a caller-supplied buffer.
 *
 * path   : path to the file
 * buf    : destination buffer
 * bufsz  : capacity of buf in bytes
 * out_sz : set to the number of bytes actually copied (≤ bufsz)
 *
 * Returns 0 on success, negative on error.
 */
int fat32_read_file(const char *path, void *buf, uint32_t bufsz,
                    uint32_t *out_sz);

/* Return 1 if path exists on the volume, 0 otherwise. No heap allocation. */
int fat32_file_exists(const char *path);

/*
 * fat32_write_file – create or overwrite a file.
 *
 * If the file already exists its cluster chain is freed and replaced.
 * If the file does not exist a new directory entry is created.
 *
 * Returns 0 on success, negative on error.
 */
int fat32_write_file(const char *path, const void *buf, uint32_t size);

/*
 * fat32_delete_file – delete a file and free its cluster chain.
 * Returns 0 on success, -1 if not found or is a directory, -2 on I/O error.
 */
int fat32_delete_file(const char *path);

/*
 * fat32_delete_dir – delete an empty directory and free its clusters.
 * Returns 0 on success, -5 if not empty, -1 if not found, -2 on I/O error.
 */
int fat32_delete_dir(const char *path);

/*
 * fat32_rename_file / fat32_rename_dir – move or rename a file or directory.
 * Creates a new directory entry at new_path pointing to the same cluster chain,
 * then marks the old entry as deleted.  new_path must not already exist.
 * Returns 0 on success, -1 if old not found, -6 if new_path exists, -2 I/O error.
 */
int fat32_rename_file(const char *old_path, const char *new_path);
int fat32_rename_dir(const char *old_path, const char *new_path);

/* -------------------------------------------------------------------------
 * Format
 * ---------------------------------------------------------------------- */

/*
 * fat32_mkfs – format a partition as FAT32.
 *
 * drive        : IDE drive number
 * part_lba     : LBA of partition start (first BPB sector)
 * part_sectors : total number of 512-byte sectors in the partition
 *
 * The partition must be large enough to hold at least 65 525 clusters
 * (≥ ~32 MiB with 1-sector clusters).  Returns 0 on success, or:
 *   -1  invalid arguments
 *   -2  I/O error
 *   -6  partition too small for FAT32
 */
int fat32_mkfs(uint8_t drive, uint32_t part_lba, uint32_t part_sectors);

/* -------------------------------------------------------------------------
 * Tab completion
 * ---------------------------------------------------------------------- */

/*
 * fat32_complete_cb_t – callback invoked for each directory entry during
 * completion scanning.  name is the entry name, is_dir is 1 for directories.
 */
typedef void (*fat32_complete_cb_t)(const char *name, int is_dir, void *ctx);

/*
 * fat32_complete – enumerate all entries in dir_path, invoking cb for each.
 * prefix is passed through to the callback context; the caller filters by it.
 * Returns 0 on success, negative on error.
 */
int fat32_complete(const char *dir_path, const char *prefix,
                   fat32_complete_cb_t cb, void *ctx);

#endif /* _KERNEL_FAT32_H */
