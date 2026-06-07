---
title: Using Makar
nav_order: 3
has_children: true
---

# Using Makar

This section covers day-to-day use inside the running OS: shells, scripting,
the BASIC interpreter, and the in-OS kernel rebuild workflow.

| Guide | What it covers |
|---|---|
| [Shell scripting](scripting.md) | The split between `/apps/sh.elf` and the in-kernel script runner, including variables, tests, control flow, userspace shell features, and limitations. |
| [BASIC](basic.md) | The `basic.elf` interpreter, REPL commands, line-numbered programs, integer expressions, graphics statements, and bundled samples. |
| [Networking](networking.md) | Viewing `eth0` state with `maknetcfg.elf`, DHCP release/renew, DNS cache flushing, and current networking limits. |
| [Rebuild kernel](rebuild-kernel.md) | Building the kernel from inside Makar, including prerequisites and workflow notes. |

For host-side build, boot, and test commands, use
[Building and running](building.md). For implementation details behind the
user-facing behavior, use [Internals](internals.md) and the
[Kernel subsystems](kernel/) reference.

## Rescue shell (single-user mode)

The GRUB entry **"Makar OS (rescue shell)"** boots with `shell=rescue` on the
kernel command line. This skips the normal multi-VT userspace boot and starts a
single bare **in-kernel** shell on VT0 — the recovery path for when
`/apps/sh.elf` or the rootfs is broken.

The rescue shell **logs in as `root` automatically with no password prompt**
(the prompt shows `root@makar:/#`). This is intentional and matches the Unix
single-user-mode trust model (Linux `init=/bin/sh`): whoever has the physical
console already has full control of the machine, so a password would only be an
obstacle during recovery, not real security. Protecting the console itself is a
bootloader-password concern (GRUB), not the OS shell.

Normal boots are unaffected — they still go through the usual login / autologin
flow (`auth_try_autologin`; see [GUI](gui.md) and `auth.h`).
