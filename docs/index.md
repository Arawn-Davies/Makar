# Makar - Documentation

Makar is a bare-metal i686 operating system written in C, booted via GRUB
Multiboot 2.  It is the C/C++ sibling of
[Medli](https://github.com/Arawn-Davies/Medli) - both implement the same OS
concept, sharing UX conventions, filesystem design, and long-term binary
format goals.  See [Makar × Medli](makar-medli.md) for the full co-operation
roadmap.

## Kernel subsystems

| Document | Description |
|---|---|
| [kernel](kernel/kernel.md) | Boot entry point and post-boot heartbeat |
| [system](kernel/system.md) | Panic, halt, and assertion helpers |
| [asm](kernel/asm.md) | Inline x86 port I/O and CPU-control helpers |
| [types](kernel/types.md) | Common type aliases and geometric structs |
| [vga](kernel/vga.md) | VGA text-mode constants and low-level helpers |
| [tty](kernel/tty.md) | VGA text terminal driver |
| [serial](kernel/serial.md) | Serial port (UART) driver |
| [descr_tbl](kernel/descr_tbl.md) | GDT and IDT initialisation |
| [isr](kernel/isr.md) | Interrupt and IRQ dispatch |
| [timer](kernel/timer.md) | PIT timer driver and `ksleep` |
| [pmm](kernel/pmm.md) | Physical memory manager |
| [paging](kernel/paging.md) | Paging and virtual memory |
| [heap](kernel/heap.md) | Kernel heap allocator (`kmalloc` / `kfree`) |
| [vesa](kernel/vesa.md) | VESA linear framebuffer driver |
| [vesa_tty](kernel/vesa_tty.md) | VESA bitmap-font text renderer |
| [vt](kernel/vt.md) | Per-TTY logical character grid (Linux `vc_data`-style backing buffer) |
| [vtty](kernel/vtty.md) | Virtual TTY manager (focus, Alt+Fn switch, deferred repaint, status bar) |
| [debug](kernel/debug.md) | INT 1 / INT 3 debug-exception handlers |
| [multiboot](kernel/multiboot.md) | Multiboot 2 structure definitions |
| [keyboard](kernel/keyboard.md) | PS/2 keyboard driver (IRQ 1, scan-code set 1, ring buffer) |
| [ide](kernel/ide.md) | ATA/IDE PIO driver (28-bit LBA read/write) |
| [partition](kernel/partition.md) | MBR and GPT partition table driver |
| [shell](kernel/shell.md) | Interactive multi-TTY kernel command shell (rm, mv, cp, …) |

| [fat32](kernel/fat32.md) | FAT32 filesystem driver (read, write, delete, rename) |
| [vfs](kernel/vfs.md) | Virtual filesystem layer (unified path namespace) |
| [procfs](kernel/procfs.md) | Synthetic `/proc` filesystem (`cpuinfo`, `meminfo`, `tasks`, `uname`) |
| [syscall](kernel/syscall.md) | int 0x80 syscall dispatcher and userspace ABI |
| [vmm](kernel/vmm.md) | Per-task page directory and ring-3 memory management |
| [task](kernel/task.md) | Cooperative round-robin task scheduler |

## Standard library (libc)

| Document | Description |
|---|---|
| [libc overview](libc/index.md) | Overview of the freestanding libc |
| [stdio](libc/stdio.md) | `printf`, `putchar`, `puts` |
| [stdlib](libc/stdlib.md) | `abort` |
| [string](libc/string.md) | Memory and string utilities |

## Makar × Medli

| Document | Description |
|---|---|
| [makar-medli](makar-medli.md) | Co-operation roadmap, shared filesystem layout, binary compatibility goals, and planned milestones |

## Build & run

| Document | Description |
|---|---|
| [Building & running](building.md) | Full build guide - native, Docker scripts, Docker Compose, environment variables, QEMU drive layout |
| [Testing](testing.md) | Serial smoke test, GDB boot-test suite, debugging with GDB |
| [WSL2 guide](wsl2.md) | Building and running on Windows via WSL2 + Docker Desktop, including QEMU GUI options |

For a quick-start see the repository [README](../README.md).
