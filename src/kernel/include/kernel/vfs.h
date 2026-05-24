#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

/*
 * vfs.h - lightweight Virtual Filesystem layer.
 *
 * Provides a single, unified path namespace:
 *
 *   /              – virtual root; rootfs election elevates a disk volume here
 *                    (live CD → /, ext2/FAT32 HDD → /).  Unix-style paths
 *                    (/usr, /etc, /home, /apps, /root) resolve transparently
 *                    via the rootfs prefix.
 *   /mnt/<name>    – user-mountable disk volumes (mkdir + mount).
 *   /dev /proc     – synthetic block-device + process trees.
 *   /tmp /log      – synthetic in-RAM writable overlays.
 *
 * All shell commands (ls, cd, cat, mkdir) use this layer so they work
 * transparently across every filesystem.
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
 * Probes every ATA drive: single-partition disks bind at /mnt/root;
 * dual-partition installer layouts bind partition 0 at /mnt/boot
 * (FAT32) and partition 1 at /mnt/root (ext2 or FAT32).  ISO9660
 * CD-ROMs are pre-registered at /mnt/cdrom by vfs_init.  The rootfs
 * election then promotes whichever volume holds /usr/lib/crt0.o to "/".
 */
void vfs_auto_mount(void);

/* -------------------------------------------------------------------------
 * State notifications (called by mount/umount commands)
 * ---------------------------------------------------------------------- */

/*
 * vfs_notify_cdrom_ejected – called after the ATAPI eject command succeeds.
 * Clears the internal CD-ROM drive reference so /mnt/cdrom is no longer
 * accessible, and resets the CWD to "/" if it was inside /mnt/cdrom.
 */
void vfs_notify_cdrom_ejected(void);

/* -------------------------------------------------------------------------
 * Current working directory
 * ---------------------------------------------------------------------- */

/* Return a pointer to the current VFS path (e.g. "/usr/lib" or "/apps"). */
const char *vfs_getcwd(void);

/*
 * Hard-disk mount table.  FAT32 (kernel + bootloader + root, EFI-style) and
 * ext2 (apps / user directories) coexist at separate /mnt/<name> mountpoints.
 *
 * vfs_mount_hd  – mount (drive, lba) at /mnt/<name>, auto-selecting the
 *                 backend (ext2 superblock preferred, else FAT32).  Returns 0
 *                 and writes the chosen backend id to *out_fs (may be NULL),
 *                 or a negative error code.
 * vfs_umount_hd – unmount /mnt/<name> (NULL/empty = the sole mount if unique).
 * vfs_hd_mounted – 1 if any HD volume is mounted.
 * vfs_hd_fsname  – backend name ("FAT32"/"ext2"/"none") for /mnt/<name>.
 */
int         vfs_mount_hd(uint8_t drive, uint32_t lba, const char *name, int *out_fs);
int         vfs_umount_hd(const char *name);
int         vfs_hd_mounted(void);
const char *vfs_hd_fsname(const char *name);

/* Empty-mountpoint management (Linux-style: a mountpoint is a directory under
 * /mnt that `mount` later binds a filesystem into).
 * vfs_make_mountpoint   – create an empty /mnt/<name> (backs `mkdir /mnt/..`).
 *                         0, -1 bad name, -6 exists, -12 table full, -13 reserved.
 * vfs_remove_mountpoint – remove an empty /mnt/<name> (backs `rmdir /mnt/..`).
 *                         0, -1 no such mountpoint, -16 busy (fs bound). */
int         vfs_make_mountpoint(const char *name);
int         vfs_remove_mountpoint(const char *name);

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

/* Lean stat -- fills only the fields the kernel can know cheaply, so
 * SYS_STAT / SYS_FSTAT don't have to eager-load the file data just to
 * read st_size.  `kind` is one of VFS_STAT_* below. */
#define VFS_STAT_FILE     1
#define VFS_STAT_DIR      2
#define VFS_STAT_BLOCKDEV 3
#define VFS_STAT_CHARDEV  4
typedef struct {
    uint32_t size;
    uint8_t  kind;   /* VFS_STAT_* */
} vfs_stat_info_t;
/* Returns 0 on success, -1 if path doesn't resolve to a known node. */
int vfs_stat(const char *path, vfs_stat_info_t *out);

/*
 * Compatibility wrappers around the synthetic /log directory (see logfs.h).
 * /log is now a writable in-RAM log tree (/log/kernel.log, /log/install.log,
 * plus any file a program writes), not a single flat file.  These shims keep
 * the historic kernel-facing klog API working by targeting /log/kernel.log,
 * the serial debug tee:
 *
 * vfs_klog_reset  – clear kernel.log.
 * vfs_klog_write  – append raw bytes to kernel.log (preserves the byte stream).
 * vfs_klog_append – append one line to kernel.log (a trailing newline added).
 *
 * The serial debug stream is tee'd into kernel.log so it survives even when an
 * operation is failing on the very volume it writes to.  Read it back with
 * `cat /log/kernel.log`.  When full a file keeps its newest output (ring).
 */
void vfs_klog_reset(void);
void vfs_klog_write(const char *s, uint32_t n);
void vfs_klog_append(const char *line);

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
