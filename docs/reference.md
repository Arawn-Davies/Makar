---
title: Reference
nav_order: 5
has_children: true
---

# Reference

This section is the technical reference for Makar's kernel, syscall ABI,
userspace C libraries, POSIX compatibility layer, and toolchain direction.

Start here when you need implementation detail rather than a quick run command.

| Area | Use it for |
|---|---|
| [Kernel subsystems](kernel/) | Per-module kernel references: descriptor tables, ISR dispatch, paging, PMM, heap, devices, filesystems, terminals, keyboard, shell, and VFS-backed pseudo-filesystems. |
| [Internals](internals.md) | Cross-cutting architecture: boot state, GDT/TSS/TLS, paging, task lifecycle, scheduler, signals, syscall dispatch, fork/exec/wait, and hosted-libc bring-up state. |
| [Syscalls](syscalls.md) | The Linux i386-compatible syscall subset plus Makar extension numbers for terminal, framebuffer, keyboard, VFS, admin, and VT operations. |
| [POSIX compatibility](posix.md) | Current POSIX-shaped behavior and known gaps for process, fd, VFS, memory, signals, time, shell, and libc work. |
| [Userland libc](userland-libc.md) | The userspace libc shim, installed sysroot, header surface, TCC integration, and static-musl bring-up status. |
| [TinyCC in Makar](tcc.md) | Shipped in-OS compiler, sysroot layout, supported workflows, test coverage, and current limits. |
| [Testing](testing.md) | In-guest test modes, expected serial markers, `run.sh` entry points, and guidance for adding coverage. |

The kernel subsystem pages are intentionally more local and file-oriented. The
top-level pages above explain behavior that crosses module boundaries.
