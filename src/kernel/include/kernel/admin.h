#ifndef _KERNEL_ADMIN_H
#define _KERNEL_ADMIN_H

#include <stdint.h>

/*
 * admin.h -- privileged operations the userspace shell can invoke.
 *
 * Each admin syscall (SYS_REBOOT, SYS_MOUNT, ...) calls task_is_admin()
 * then delegates to one of these helpers.  Keeping them out of the
 * shell module means the userspace shell, the in-kernel rescue shell,
 * and any future login/sudo path all reach the same code.
 *
 * Return convention:
 *   0       success
 *   <0      negative errno-style (-1 = generic, -EPERM = -1 here too)
 *
 * Functions marked __attribute__((noreturn)) (admin_shutdown,
 * admin_reboot) only return if the privilege check fails.
 */

/* Power. */
int admin_shutdown(void);
int admin_reboot(void);

/* Display. */
int admin_setmode(const char *mode);
int admin_fgcol(const char *colour);
int admin_bgcol(const char *colour);

/* Storage. */
int admin_mount(const char *dev_path, const char *mnt_path);
int admin_umount(const char *target);   /* target may be NULL */
int admin_mkfs(const char *dev_path, const char *fstype);  /* "ext2"|"fat32" */
int admin_eject(void);                   /* unmount + open CD-ROM tray */
int admin_install(void);                 /* run installer TUI (Limine MBR + rootfs copy) */

/* Scheduler / runtime tuning. */
int admin_sched_quantum(int new_value);  /* <0 = query; returns current */

/* Linux-style runtime opt-in for tty-to-serial mirror.
 * arg: 1 = on, 0 = off, -1 = query.  Returns the post-call state. */
int admin_verbose(int onoff);

#endif /* _KERNEL_ADMIN_H */
