---
title: Home
layout: default
nav_order: 1
permalink: /
---

# Makar

A **bare-metal i686 hobby OS** written in straight C (no C++, no managed
runtime), booted via GRUB Multiboot 2. Targets 32-bit protected mode,
ships in QEMU, runs on real hardware once installed to disk.

Sibling of [Medli](https://github.com/Arawn-Davies/Medli) — the
C# / Cosmos counterpart. The two share a command vocabulary,
filesystem layout, and long-term binary-format goals while exploring
how language choice shapes the implementation.

**Current version:** 0.5.0 (see
[`include/kernel/version.h`](https://github.com/Arawn-Davies/Makar/blob/main/src/kernel/include/kernel/version.h)).

## Quick links

- **[Building & running](building.md)** — toolchain, Docker, QEMU
- **[Testing](testing.md)** — ktest, GDB checkpoint suite, UI sendkey tests
- **[Userland libc](userland-libc.md)** — porting roadmap toward musl/dash
- **[Makar × Medli](makar-medli.md)** — sibling-project co-operation roadmap and VIX→VICS history
- **[Repo on GitHub](https://github.com/Arawn-Davies/Makar)**

## What works today

| Subsystem | State |
|---|---|
| **Boot** | GRUB Multiboot 2, 5 s menu (Makar OS / chainload next device). Cmdline parsed for `test_mode` + `console=ttyS0`. |
| **Display** | VESA framebuffer (Bochs VBE, 720p default); VGA 80×50 fallback. `vesa_pane_t` pane abstraction. |
| **Multi-TTY** | 4 preemptive shell tasks `shell0`–`shell3`, **Alt+F1–F4** to switch. Per-TTY `vt_buf_t` backing grid; FB painted only when focused; tmux-style status bar at bottom row. |
| **VIX editor** | vim-style line-number gutter, word wrap, flashing block caret, status row, runtime-resolution agnostic. Renamed from VICS during the port — see [Makar × Medli](makar-medli.md). |
| **Storage** | FAT32 (HDD/USB) + ISO 9660 (CD-ROM) over IDE PIO. Auto-mount at `/hd` and `/cdrom`. Read + write + delete + rename on FAT32. |
| **`/proc`** | Synthetic filesystem with `cpuinfo`, `meminfo`, `tasks`, `uname` — content generated on each read. |
| **Memory** | PMM bitmap allocator, paging (256 MiB identity + per-task 4 KiB user pages), kernel heap. |
| **Tasking** | Preemptive round-robin scheduler. PIT 100 Hz, `SCHED_QUANTUM = 4` ticks (40 ms slice). Per-task `pid`, `cwd`, `tty`, fd-table placeholder, signal bitmasks. User PD reaped on task exit. |
| **Userspace** | Ring-3 via `iret`. ELF loader (`elf_exec`) with argc/argv. Apps: `hello`, `echo`, `calc`, `ls`, `vix`, `diskinfo`, `rm`, `mv`, `cp`, `kbtester`. |
| **Syscalls** | Linux i386 ABI subset over `int 0x80` + Makar extensions (211–214). |
| **Shell** | Inline editing, 16-entry history, cross-FS tab completion, glob expansion, Ctrl+C sigint, `lsman`/`man <cmd>`. Fullscreen-command dispatch with auto FB restore. |
| **Drivers** | 16550 UART, PIT, layered PS/2 keyboard (full set-1 + e0 with per-task SPSC rings), ATA/IDE PIO 28-bit LBA, MBR + GPT partition tables. |
| **Debug** | INT 1 / INT 3 GDB-friendly handlers, kernel panic screen, ktest harness with per-suite descriptions and VGA + serial output. |
| **Serial** | Linux-style: dmesg + explicit diagnostics by default. `console=ttyS0` cmdline or `verbose [on\|off]` shell builtin opts into TTY-mirroring. |

## Kernel subsystem reference

Per-driver and per-module documentation:

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
| [keyboard](kernel/keyboard.md) | PS/2 keyboard driver (layered, IRQ 1, set 1 + e0, per-task SPSC rings) |
| [ide](kernel/ide.md) | ATA/IDE PIO driver (28-bit LBA read/write) |
| [partition](kernel/partition.md) | MBR and GPT partition table driver |
| [procfs](kernel/procfs.md) | Synthetic `/proc` filesystem |
| [shell](kernel/shell.md) | Interactive multi-TTY kernel command shell |

## Standard library

| Document | Description |
|---|---|
| [libc overview](libc/index.md) | Freestanding libc (`libk.a`) |
| [stdio](libc/stdio.md) | `printf`, `putchar`, `puts` |
| [stdlib](libc/stdlib.md) | `abort` |
| [string](libc/string.md) | Memory and string utilities |

## Source map

```
src/kernel/arch/i386/
  boot/       Multiboot 2 entry, crti/crtn
  core/       GDT/IDT, ISR stub, interrupt dispatch
  mm/         pmm.c, paging.c, vmm.c, heap.c
  drivers/    serial, keyboard, timer, IDE, ACPI, partition
  fs/         fat32.c, iso9660.c, procfs.c, vfs.c
  display/    tty.c, vesa.c + vesa_tty.c, vt.c
  proc/       task.c + task_asm.S, syscall.c, ring3.S, vix.c, vtty.c
  shell/      shell.c, shell_cmd_*.c
  debug/      exception handlers
src/kernel/kernel/kernel.c    kernel_main
src/kernel/include/kernel/    public headers
src/libc/                     freestanding libc → libk.a
src/userspace/                ring-3 ELF apps
tests/                        gdb_boot_test.py, ui_test.sh
```

## Acknowledgements

Makar draws on the work of many FOSS projects. Full attribution in
[LICENSES/THANKS.md](https://github.com/Arawn-Davies/Makar/blob/main/LICENSES/THANKS.md)
and per-file source-level credits. Key influences:

- **Linux kernel** (GPLv2) — syscall ABI, ELF loading model, process memory layout
- **ELKS / FUZIX** (GPLv2) — minimal-libc / crt0 approach; vi-style editor design
- **CP/M** — terminal-owns-screen philosophy; self-contained program model
- **musl libc** (MIT) — target libc for the userspace porting roadmap
- **GRUB** (GPLv2) — bootloader and Multiboot 2 tag format
- **OSDev wiki** (CC-BY-SA) — cross-compiler setup, paging, descriptor tables
