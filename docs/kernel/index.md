---
title: Kernel subsystems
parent: Reference
nav_order: 4
has_children: true
permalink: /kernel/
---

# Kernel subsystems

Per-driver and per-module reference documentation for Makar's kernel. These
pages are organized around source ownership: each page explains the public
interfaces, invariants, and current behavior of one subsystem rather than
retelling the whole OS architecture.

For cross-subsystem topics, use:

| Topic | Reference |
|---|---|
| Process lifecycle, scheduler, signals, fork/exec/wait | [Internals](../internals.md) |
| Syscall numbers and ABI registers | [Syscalls](../syscalls.md) |
| POSIX compatibility state | [POSIX](../posix.md) |
| In-guest test matrix | [Testing](../testing.md) |

| Document | Description |
|---|---|
| [kernel](kernel.md) | Boot entry point and post-boot heartbeat |
| [system](system.md) | Panic, halt, and assertion helpers |
| [asm](asm.md) | Inline x86 port I/O and CPU-control helpers |
| [types](types.md) | Common type aliases and geometric structs |
| [vga](vga.md) | VGA text-mode constants and low-level helpers |
| [tty](tty.md) | VGA text terminal driver |
| [serial](serial.md) | Serial port (UART) driver |
| [descr_tbl](descr_tbl.md) | GDT, IDT, TSS, and TLS descriptor setup |
| [isr](isr.md) | Interrupt, exception, IRQ, and syscall dispatch |
| [timer](timer.md) | PIT timer driver |
| [pmm](pmm.md) | Physical memory manager |
| [paging](paging.md) | Paging, COW faults, and user virtual memory |
| [heap](heap.md) | Kernel heap allocator |
| [vesa](vesa.md) | VESA linear framebuffer |
| [vesa_tty](vesa_tty.md) | VESA bitmap-font text renderer |
| [vt](vt.md) | Per-TTY logical character grid |
| [vtty](vtty.md) | Virtual TTY manager (Alt+Fn, focus, status bar) |
| [debug](debug.md) | Debug-exception handlers |
| [multiboot](multiboot.md) | Multiboot 2 structures |
| [keyboard](keyboard.md) | Layered PS/2 keyboard driver |
| [ide](ide.md) | ATA/IDE PIO driver |
| [partition](partition.md) | MBR + GPT partition tables |
| [ext2](ext2.md) | ext2 filesystem driver (read + write + mkfs) |
| [procfs](procfs.md) | Synthetic `/proc` filesystem |
| [devfs](devfs.md) | Synthetic `/dev` block devices (disks, partitions, CD-ROM) |
| [shell](shell.md) | Kernel command shell, builtins, and script dispatcher |
