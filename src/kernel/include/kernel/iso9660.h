#ifndef _KERNEL_ISO9660_H
#define _KERNEL_ISO9660_H

/*
 * iso9660.h - read-only ISO9660 filesystem driver public API.
 *
 * All I/O is delegated to ide_read_atapi_sectors() (2048-byte CD sectors).
 * The driver is stateless; the ATAPI drive number is passed to every call.
 * Rock Ridge extensions are not required; plain Joliet/ISO9660 level 1 is
 * sufficient for GRUB-generated rescue ISOs.
 */

#include <kernel/types.h>

/* Logical block size of an ISO9660 volume (bytes). */
#define ISO9660_SECTOR_SIZE  2048u

/*
 * iso9660_probe – verify that 'drive' contains a valid ISO9660 volume.
 *
 * Returns  0 on success.
 * Returns -1 on I/O error.
 * Returns -2 if the drive is not ATAPI or does not contain ISO9660.
 */
int iso9660_probe(uint8_t drive);

/*
 * iso9660_read_file – read a file from an ISO9660 volume into 'buf'.
 *
 * path   : absolute path with '/' separators (e.g. "/boot/makar.kernel")
 * buf    : destination buffer
 * bufsz  : capacity of 'buf' in bytes
 * out_sz : set to the number of bytes actually copied (≤ bufsz)
 *
 * Returns  0 on success.
 * Returns -1 on I/O error.
 * Returns -2 if the path is not found or resolves to a directory.
 */
int iso9660_read_file(uint8_t drive, const char *path,
                      void *buf, uint32_t bufsz, uint32_t *out_sz);

/*
 * iso9660_ls – list the contents of a directory to the terminal.
 *
 * Returns  0 on success.
 * Returns -1 on I/O error.
 * Returns -2 if the path is not found or is not a directory.
 */
/* Return 1 if path exists on the volume, 0 otherwise. No heap allocation. */
int iso9660_file_exists(uint8_t drive, const char *path);

int iso9660_ls(uint8_t drive, const char *path);

/*
 * iso9660_complete – enumerate entries in dir_path, calling cb for each.
 * Mirrors fat32_complete's signature so the VFS layer can route uniformly.
 * Caller is responsible for filtering by 'prefix' inside the callback.
 *
 * Returns  0 on success.
 * Returns -1 on I/O error.
 * Returns -2 if dir_path does not resolve to a directory.
 */
typedef void (*iso9660_complete_cb_t)(const char *name, int is_dir, void *ctx);
int iso9660_complete(uint8_t drive, const char *dir_path, const char *prefix,
                     iso9660_complete_cb_t cb, void *ctx);

#endif /* _KERNEL_ISO9660_H */
