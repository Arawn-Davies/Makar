#ifndef _KERNEL_INSTALLER_H
#define _KERNEL_INSTALLER_H

/*
 * installer.h - OS-to-disk installer public API.
 *
 * installer_run() performs a complete interactive (TUI) installation:
 *
 *  1.  Detect source ISO9660 CD-ROM (ATAPI drive).
 *  2.  Full-screen menus: pick target ATA drive, root filesystem
 *      (ext2 / FAT32), and partition mode (whole-disk auto / cfdisk).
 *  3.  Confirm the destructive operation ("type yes").
 *  4.  Partition the drive (one rootfs partition spanning the disk).
 *  5.  Format it (ext2_mkfs / fat32_mkfs) and mount it.
 *  6.  Copy the kernel to /boot, and the /apps, /docs and /src trees.
 *  7.  Drop limine-bios.sys + a generated limine.conf into /limine.
 *  8.  Install limine to the MBR: boot sector -> LBA0 (partition table
 *      preserved), stage 2 -> post-MBR gap, stage-2 offset patched at 0x1a4.
 *  9.  Unmount and report success.
 *
 * Rendering uses the VESA TTY (reserving the makmux status row); VGA-text
 * mode degrades to numbered prompts.  The host build still boots via GRUB;
 * only this runtime installer deploys limine.
 */

/*
 * installer_run – run the interactive OS installation wizard.
 * Does not return until the installation completes or is cancelled.
 */
void installer_run(void);

#endif /* _KERNEL_INSTALLER_H */
