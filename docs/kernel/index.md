---
title: Kernel subsystems
nav_order: 10
has_children: true
permalink: /kernel/
---

# Kernel subsystems

Per-driver and per-module reference documentation for Makar's kernel.

| Document | Description |
|---|---|
| [kernel](kernel.md) | Boot entry point and post-boot heartbeat |
| [system](system.md) | Panic, halt, and assertion helpers |
| [asm](asm.md) | Inline x86 port I/O and CPU-control helpers |
| [types](types.md) | Common type aliases and geometric structs |
| [vga](vga.md) | VGA text-mode constants and low-level helpers |
| [tty](tty.md) | VGA text terminal driver |
| [serial](serial.md) | Serial port (UART) driver |
| [descr_tbl](descr_tbl.md) | GDT and IDT initialisation |
| [isr](isr.md) | Interrupt and IRQ dispatch |
| [timer](timer.md) | PIT timer driver |
| [pmm](pmm.md) | Physical memory manager |
| [paging](paging.md) | Paging and virtual memory |
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
| [procfs](procfs.md) | Synthetic `/proc` filesystem |
| [shell](shell.md) | Interactive multi-TTY kernel command shell |
