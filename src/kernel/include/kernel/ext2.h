#ifndef _KERNEL_EXT2_H
#define _KERNEL_EXT2_H

/*
 * ext2.h - ext2 filesystem driver public API.
 *
 * Mirrors the FAT32 driver surface (kernel/fat32.h) so the VFS layer can
 * dispatch the single /mnt/<name> hard-disk volume to either backend.
 * One mounted volume at a time.  Paths use '/' separators and are
 * case-sensitive (unlike FAT32).  All sector I/O is delegated to
 * ide_read_sectors() / ide_write_sectors().
 *
 * Supports rev 0 and rev 1 (dynamic) ext2, block sizes 1024/2048/4096,
 * the FILETYPE incompat feature, and read/write of regular files via
 * direct + single-indirect + double-indirect block maps.
 */

#include <kernel/types.h>
#include <kernel/fat32.h>   /* fat32_complete_cb_t */

/* -------------------------------------------------------------------------
 * Mount / unmount
 * ---------------------------------------------------------------------- */

/*
 * ext2_probe – cheap check: is there an ext2 superblock at this partition?
 * Reads the 1024-byte-offset superblock and tests the magic.  No state
 * change.  Returns 1 if ext2, 0 otherwise.
 */
int ext2_probe(uint8_t drive, uint32_t part_lba);

/*
 * ext2_mount – mount an ext2 partition.
 *   drive    : IDE drive number (0-3)
 *   part_lba : LBA of the first sector of the partition
 * Returns 0 on success, negative on error (-2 = not ext2, -3 = unsupported
 * incompat feature, -1 = I/O error).
 */
int ext2_mount(uint8_t drive, uint32_t part_lba);

/*
 * ext2_mkfs – format a partition as ext2 (1 KiB blocks, rev 1 with FILETYPE).
 *   drive        : IDE drive number
 *   part_lba     : LBA of partition start
 *   part_sectors : partition length in 512-byte sectors
 * Creates the superblock, per-group descriptors/bitmaps/inode tables, the
 * root directory and lost+found.  Returns 0, -6 (too small), or -2 (I/O).
 */
int ext2_mkfs(uint8_t drive, uint32_t part_lba, uint32_t part_sectors);

/* Flush metadata and unmount the current volume. */
void ext2_unmount(void);

/* Returns 1 if an ext2 volume is currently mounted, 0 otherwise. */
int ext2_mounted(void);

/* Filesystem usage in KiB (total + free) of the mounted ext2 volume.
 * Returns 0 on success, -1 if not mounted. */
int ext2_statfs(uint32_t *total_kb, uint32_t *free_kb);

/* -------------------------------------------------------------------------
 * Directory operations
 * ---------------------------------------------------------------------- */
int ext2_ls(const char *path);
int ext2_cd(const char *path);
int ext2_mkdir(const char *path);

/* -------------------------------------------------------------------------
 * File I/O
 * ---------------------------------------------------------------------- */
int ext2_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz);
/* Byte-range read for demand-paged / page-cache file I/O.  Returns bytes read
 * (>=0, 0 at/past EOF) or -1 on error. */
long ext2_read_at(const char *path, uint32_t off, void *buf, uint32_t len);
int ext2_file_exists(const char *path);
/* Lean stat: fills *out_size and *out_is_dir without reading file data.
 * Returns 0 on success, -1 if path unresolved or volume unmounted. */
int ext2_stat(const char *path, uint32_t *out_size, int *out_is_dir);
int ext2_write_file(const char *path, const void *buf, uint32_t size);
int ext2_delete_file(const char *path);
int ext2_delete_dir(const char *path);
int ext2_rename_file(const char *old_path, const char *new_path);
int ext2_rename_dir(const char *old_path, const char *new_path);

/* -------------------------------------------------------------------------
 * Tab completion (shares the FAT32 callback type)
 * ---------------------------------------------------------------------- */
int ext2_complete(const char *dir_path, const char *prefix,
                  fat32_complete_cb_t cb, void *ctx);

#endif /* _KERNEL_EXT2_H */
