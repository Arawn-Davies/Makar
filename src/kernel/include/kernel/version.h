#ifndef _KERNEL_VERSION_H
#define _KERNEL_VERSION_H

/*
 * Single source of truth for Makar's version strings.  Bumped per
 * SemVer when a significant slice ships.  Consumed by the shell welcome
 * banner, the `version` builtin, /proc/uname, and anywhere else that
 * wants to report a version.
 *
 *   MAKAR_VERSION  -- the kernel itself.  0.9 marks the kernel self-host
 *                     milestone: the bootable Multiboot 2 kernel ELF is
 *                     built end-to-end with our shipped TCC against the
 *                     vendored source tree (host-side proven, full in-OS
 *                     path runs and produces REBUILD-KERNEL: ALL PASS
 *                     when all per-file compiles succeed).  v1.0 stays
 *                     reserved for self-hosting PLUS the polish
 *                     phase -- full-green tests, hardened test harness,
 *                     docs at 100%, and the "10× dev experience" pass.
 *                     Stays at 0.9.x with patch bumps through that
 *                     polish work.  0.8 was the in-OS TCC milestone
 *                     (calc.elf + sh.elf rebuild themselves under TCC).
 *   SHELL_VERSION  -- the in-kernel shell (scripting + REPL).  Tracked
 *                     separately so shell-only changes don't move the
 *                     kernel version and vice versa.
 */
/* 0.10 -- the makx milestone: GUI re-architected into an X11-style display
 *         server + client apps over IPC + shared surfaces; syscall ABI
 *         consolidated into one source of truth; hypervisor detection +
 *         per-platform fixes (Hyper-V mouse, VMware/Hyper-V 720p, ACPI soft-off);
 *         windowed clients re-flow on resize. */
#define MAKAR_VERSION "0.10.5"
/* 0.9 -- ring-3 shell driving the makx desktop session (GUI login hand-off,
 *        focus-gain prompt rebuild, autostart). */
#define SHELL_VERSION "0.9.0"

#endif /* _KERNEL_VERSION_H */
