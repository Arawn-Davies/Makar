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
 * installer_run – run the interactive TUI installation wizard (shell mode).
 * Collects all choices via the ANSI text UI, then runs the shared execution
 * engine below.  Does not return until the install completes or is cancelled.
 */
void installer_run(void);

/* ------------------------------------------------------------------------
 * Headless execution engine (shared by the TUI wizard and the GUI installer).
 *
 * The TUI installer collects these choices with its text wizard; the GUI
 * installer (mxinstall.elf) collects them graphically.  Both then drive the
 * same three-phase engine.  Execution is *stepped* -- the caller copies one
 * file per install_exec_step() call -- so a GUI front-end can repaint a
 * progress bar and yield between files instead of freezing behind one giant
 * blocking syscall.
 * ------------------------------------------------------------------------ */

/* install_params_t / install_progress_t / install_drive_t + INSTALL_* live in
 * the shared kernel<->user ABI header so the GUI client agrees byte-for-byte. */
#include <makar_abi.h>

/* Enumerate selectable ATA target drives into out[INSTALL_MAX_DRIVES].
 * Returns the count.  (The GUI installer can't reach ide_get_drive directly.) */
int install_exec_drives(install_drive_t *out);

/* Phase 1: partition + mkfs both partitions + limine MBR + boot files +
 * account/hostname metadata; leaves the data partition mounted for copying.
 * Returns 0 on success, negative on failure. */
int install_exec_begin(const install_params_t *p);

/* Phase 2: copy the next file of the /apps,/docs,/src,/usr trees into the
 * mounted data partition.  Returns 1 while work remains (prog->done==0), 0 when
 * the last file has been copied (prog->done==1), negative on error.  Call
 * repeatedly; the caller repaints + yields between calls. */
int install_exec_step(install_progress_t *prog);

/* Phase 3: finalise (mkdir /bin, unmount).  Returns 0 on success.  After this
 * the caller reboots. */
int install_exec_finish(const install_params_t *p);

#endif /* _KERNEL_INSTALLER_H */
