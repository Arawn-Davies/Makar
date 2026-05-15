#ifndef _KERNEL_VERSION_H
#define _KERNEL_VERSION_H

/*
 * Single source of truth for Makar's version string.  Bumped per
 * SemVer when a significant slice ships.  Consumed by the shell welcome
 * banner, the `version` builtin, /proc/uname, and anywhere else that
 * wants to report the kernel version.
 */
#define MAKAR_VERSION "0.5.0"

#endif /* _KERNEL_VERSION_H */
