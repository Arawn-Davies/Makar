#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

/*
 * vfs.h - lightweight Virtual Filesystem layer.
 *
 * Provides a single, unified path namespace:
 *
 *   /          – virtual root; ls shows available mount-points
 *   /hd/…      – FAT32 hard-disk partition (mounted via cmd_mount)
 *   /cdrom/…   – ISO9660 CD-ROM (auto-detected on vfs_init)
 *
 * All shell commands (ls, cd, cat, mkdir) use this layer so they work
 * transparently across both filesystems.
 */

#include <kernel/types.h>
#include <kernel/fat32.h>

/* Maximum length of any VFS path string (including NUL terminator). */
#define VFS_PATH_MAX  256

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/*
 * vfs_init – probe IDE bus for an ISO9660 CD-ROM and reset the CWD to "/".
 * Must be called after ide_init().
 */
void vfs_init(void);

/*
 * vfs_set_boot_drive – record the BIOS drive number GRUB booted from.
 *
 * Must be called (once) before vfs_auto_mount().  The value comes from the
 * Multiboot 2 boot device tag (tag type 5).  Pass 0xFF if the tag is absent.
 *
 * BIOS numbering: 0x00–0x7F floppy, 0x80–0xDF HDD, 0xE0–0xFF CD-ROM.
 */
void vfs_set_boot_drive(uint32_t biosdev);

/*
 * vfs_auto_mount – automatically mount the appropriate filesystem.
 *
 * Must be called after both vfs_init() and ide_init().
 *
 * - BIOS HDD (0x80–0xDF): mounts the first FAT32 partition found on the
 *   corresponding ATA drive as /hd and navigates there.
 * - BIOS CD-ROM (0xE0–0xFF): the ISO9660 drive is already accessible at
 *   /cdrom (registered by vfs_init); navigates CWD there.
 * - Unknown (0xFF): tries HDD drives first, then falls back silently.
 */
void vfs_auto_mount(void);

/* -------------------------------------------------------------------------
 * State notifications (called by mount/umount commands)
 * ---------------------------------------------------------------------- */

/*
 * vfs_notify_hd_mounted   – FAT32 volume has just been mounted.
 *                           Moves the CWD to "/hd" when currently at "/".
 * vfs_notify_hd_unmounted – FAT32 volume has just been unmounted.
 *                           Resets the CWD to "/" when it was under "/hd".
 */
void vfs_notify_hd_mounted(void);
void vfs_notify_hd_unmounted(void);

/*
 * vfs_notify_cdrom_ejected – called after the ATAPI eject command succeeds.
 * Clears the internal CD-ROM drive reference so /cdrom is no longer accessible,
 * and resets the CWD to "/" if it was inside /cdrom.
 */
void vfs_notify_cdrom_ejected(void);

/* -------------------------------------------------------------------------
 * Current working directory
 * ---------------------------------------------------------------------- */

/* Return a pointer to the current VFS path (e.g. "/mnt/hd/boot/grub"). */
const char *vfs_getcwd(void);

/* Get/set the /mnt component the single FAT32 volume is reachable at.
 * Default "hd".  `mount /dev/hdaN /mnt/<name>` sets it; umount resets it.
 * `name` must be a single component (no '/'); empty/NULL resets to "hd". */
void        vfs_set_hd_mount(const char *name);
const char *vfs_hd_mount(void);

/* Flush and unmount every writable volume in preparation for power-off
 * or reset, so no dirty FAT/dir data is lost.  Safe to call when nothing
 * is mounted.  Invoked from the shutdown / reboot paths. */
void vfs_prepare_shutdown(void);

/* -------------------------------------------------------------------------
 * Filesystem operations
 * ---------------------------------------------------------------------- */

/*
 * vfs_ls    – list directory contents.  NULL → use CWD.
 * vfs_cd    – change the current VFS working directory.
 * vfs_cat   – read and print a file's contents to the terminal.
 * vfs_mkdir – create a directory (FAT32 only).
 *
 * All functions return 0 on success, negative on error.
 */
int vfs_ls(const char *path);
int vfs_cd(const char *path);
int vfs_cat(const char *path);
int vfs_mkdir(const char *path);

/*
 * vfs_read_file  – read a file into a caller-supplied buffer.
 * vfs_write_file – create or overwrite a file (FAT32 only).
 *
 * Return 0 on success, negative on error.
 */
int vfs_read_file(const char *path, void *buf, uint32_t bufsz, uint32_t *out_sz);
int vfs_write_file(const char *path, const void *buf, uint32_t size);

/* Return 1 if path exists and is readable, 0 otherwise. */
int vfs_file_exists(const char *path);

/*
 * vfs_blockdev_lookup – if path resolves to a /dev block device, return
 * its devfs node index (>= 0) and write the device's byte size into
 * *size_out.  Returns -1 if path is not a block device.  Lets the fd
 * layer open /dev nodes without eager-buffering (FD_KIND_BLOCKDEV).
 */
int vfs_blockdev_lookup(const char *path, uint32_t *size_out);

/*
 * vfs_blockdev_pread / vfs_blockdev_pwrite – byte-addressed I/O against
 * an open block-device node (as returned by vfs_blockdev_lookup).
 * Return bytes transferred, or -1 on error.
 */
long vfs_blockdev_pread(int node, void *buf, uint32_t len, uint32_t off);
long vfs_blockdev_pwrite(int node, const void *buf, uint32_t len, uint32_t off);

/*
 * vfs_delete_file – delete a file (FAT32 only).
 * vfs_delete_dir  – delete an empty directory (FAT32 only).
 * vfs_rename      – move or rename a file or directory (FAT32 only).
 *
 * All return 0 on success, negative on error.
 */
int vfs_delete_file(const char *path);
int vfs_delete_dir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);

/*
 * vfs_complete – enumerate directory entries for tab completion.
 * dir    : VFS path to enumerate (NULL → CWD).
 * prefix : passed through to cb context for caller-side filtering.
 * cb     : called for each entry (name, is_dir, ctx).
 * ctx    : opaque pointer forwarded to cb.
 * Returns 0 on success, -1 on error.
 */
int vfs_complete(const char *dir, const char *prefix,
                 fat32_complete_cb_t cb, void *ctx);

#endif /* _KERNEL_VFS_H */
