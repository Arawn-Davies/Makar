#ifndef _KERNEL_INSTALLER_H
#define _KERNEL_INSTALLER_H

/*
 * installer.h - OS-to-disk installer public API.
 *
 * installer_run() performs a complete interactive installation:
 *
 *  1.  Detect source ISO9660 CD-ROM (ATAPI drive).
 *  2.  Prompt user to select a target ATA hard drive.
 *  3.  Confirm destructive operation.
 *  4.  Install GRUB: write patched boot.img to MBR, write core.img to the
 *      pre-partition embedding area (sectors 1..N).
 *  5.  Partition the target drive (one FAT32 LBA partition).
 *  6.  Format the partition as FAT32 (fat32_mkfs).
 *  7.  Mount the new FAT32 volume.
 *  8.  Create /boot/grub/i386-pc directory tree.
 *  9.  Copy the kernel (/boot/makar.kernel) from the ISO.
 * 10.  Copy essential GRUB modules from the ISO.
 * 11.  Write /boot/grub/grub.cfg.
 * 12.  Unmount and report success.
 *
 * All user I/O is performed via the PS/2 keyboard and the VGA/VESA terminal.
 */

/*
 * installer_run – run the interactive OS installation wizard.
 * Does not return until the installation completes or is cancelled.
 */
void installer_run(void);

#endif /* _KERNEL_INSTALLER_H */
