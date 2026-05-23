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
 *                     buildable).  0.8 marks the in-OS TCC milestone:
 *                     tcc.elf runs on bare metal, calc.elf + sh.elf
 *                     rebuild themselves, ring-3 page faults SIGSEGV
 *                     cleanly, HDD root layout, ring-3 sh.elf MVP.
 *   SHELL_VERSION  -- the in-kernel shell (scripting + REPL).  Tracked
 *                     separately so shell-only changes don't move the
 *                     kernel version and vice versa.  Bumped half a
 *                     step (0.6.0 → 0.6.5) for zsh-style tab cycling.
 */
#define MAKAR_VERSION "0.8.0"
#define SHELL_VERSION "0.6.5"

#endif /* _KERNEL_VERSION_H */
