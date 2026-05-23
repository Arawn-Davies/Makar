#ifndef _KERNEL_VERSION_H
#define _KERNEL_VERSION_H

/*
 * Single source of truth for Makar's version strings.  Bumped per
 * SemVer when a significant slice ships.  Consumed by the shell welcome
 * banner, the `version` builtin, /proc/uname, and anywhere else that
 * wants to report a version.
 *
 *   MAKAR_VERSION  -- the kernel itself.  v1.0 is reserved for full
 *                     self-hosting (in-OS compiler + installable +
 *                     buildable); 0.8 is reserved for the in-OS TCC
 *                     milestone.
 *   SHELL_VERSION  -- the in-kernel shell (scripting + REPL).  Tracked
 *                     separately so shell-only changes don't move the
 *                     kernel version and vice versa.
 */
#define MAKAR_VERSION "0.7.5"
#define SHELL_VERSION "0.6.0"

#endif /* _KERNEL_VERSION_H */
