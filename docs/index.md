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

**Current version:** 0.9.5 (kernel self-host milestone; see
[`include/kernel/version.h`](https://github.com/Arawn-Davies/makar/blob/main/src/kernel/include/kernel/version.h)
and [kernel rebuild guide](rebuild-kernel.md)).

## Quick links

- **[Building & running](building.md)** — toolchain, Docker, QEMU
- **[Internals](internals.md)** — CPU state at boot, paging, TLBs, per-task PDs, scheduler, syscall ABI, fork+COW, execve, wait4
- **[Testing](testing.md)** — ktest, GDB checkpoint suite, in-guest script drivers + key-injection tests
- **[BASIC](basic.md)** — the C64-style integer BASIC interpreter (`basic.elf`)
- **[Userland libc](userland-libc.md)** — current userspace libc shim and static-musl bring-up state
- **[TinyCC in Makar](tcc.md)** — shipped in-OS compiler, sysroot, supported workflows, and limits
- **[Rebuilding the kernel inside Makar](rebuild-kernel.md)** — `/apps/rebuild-kernel.sh`, build-origin banners, installing the freshly built kernel
- **[Makar × Medli](makar-medli.md)** — sibling-project co-operation roadmap and VIX→VICS history
- **[Glossary](glossary.md)** — every abbreviation (PAT, MTRR, WC, ACPI, DISPI, VBE…) defined
- **[Repo on GitHub](https://github.com/Arawn-Davies/makar)**

## What works today

| Subsystem | State |
|---|---|
| **Boot** | GRUB Multiboot 2, 5 s menu (Makar OS / chainload next device). Cmdline parsed for `test_mode` + `console=ttyS0`. |
| **Display** | VESA framebuffer (Bochs VBE, 720p default); VGA 80×50 fallback. `vesa_pane_t` pane abstraction. |
| **Multi-TTY** | 4 preemptive shell tasks `shell0`–`shell3`, **Alt+F1–F4** to switch. Per-TTY `vt_buf_t` backing grid; FB painted only when focused; **makmux** multiplexer's status bar at the bottom row (left label, centred `VT1 VT2 VT3 VT4`, `Alt+F1-F4` hint; **Alt+F5** toggles the label between `Makar` and a live `HH:MM:SS DD/MM/YY` clock). Framebuffer syscalls are focus-gated so a backgrounded fullscreen app stays isolated to its VT. |
| **VIX editor** | vim-style line-number gutter, word wrap, flashing block caret (`SYS_CARET_STYLE`), status row, runtime-resolution agnostic. Now a **userland** ELF (`vix.elf`) run via PATH — appears in `maktop` as its own task. Renamed from VICS during the port — see [Makar × Medli](makar-medli.md). |
| **Storage** | FAT32 + **ext2** (HDD/USB) + ISO 9660 (CD-ROM) over IDE PIO, routed via a real VFS mount table (`s_mounts[]` in `vfs.c`; longest-prefix-match dispatch). Rootfs election (`vfs_mount_root`) honours `root=/dev/hdaN` Multiboot2 cmdline first, else auto-detects (ext2 → FAT32 → CD-ROM emergency) by probing each candidate for `/usr/lib/crt0.o`. Single-partition disks auto-mount at `/mnt/root`; dual-partition installer layouts bind partition 0 at `/mnt/boot` (also mirrored at `/boot`) and partition 1 at `/mnt/root`; CD-ROM at `/mnt/cdrom`. Synthetic overlays (`/dev` `/proc` `/tmp` `/log`) are first-class mount-table entries. `mount /dev/hdaN /mnt/<name>` auto-detects the backend (ext2 superblock else FAT32). FAT32 and ext2 mount **simultaneously** at separate mountpoints (one of each — the drivers are single-volume). Read + write + delete + rename + mkdir on both; `mkfs.fat32` / `mkfs.ext2` to format; flush + unmount on shutdown/reboot. `/root` mkdir'd best-effort on writable rootfs boots. |
| **`/dev`** | Synthetic block-device tree: `/dev/hda[N]` (ATA disks + partitions), `/dev/cdrom` (ATAPI). Byte-addressed read/write over native sector I/O; backs `fdisk.elf` / `cfdisk.elf` and `mount`. |
| **`/proc`** | Synthetic filesystem with `cpuinfo`, `meminfo`, `tasks`, `uname`, `rtc` — content generated on each read. `meminfo` MemUsed folds in heap; `tasks` has a per-task `MEMKB` column. |
| **Memory** | PMM bitmap allocator, paging (low identity map + per-task 4 KiB user pages), kernel heap, COW user pages, anonymous mmap window at `0x90000000`, per-task `brk`. |
| **FPU/TLS** | x87/SSE initialized at boot; every task has a 512-byte FXSAVE area saved/restored across context switches. i386 TLS uses GDT slot 6 (`%gs = 0x33`) via `set_thread_area`; ISR/IRQ stubs preserve `%gs`. |
| **Tasking** | Preemptive round-robin scheduler. PIT 100 Hz, `SCHED_QUANTUM = 4` ticks (40 ms slice). Per-task `pid`, `parent_pid`, `cwd`, `tty`, `fd_table_t`, signal bitmasks, FPU state, TLS fields, `exit_status`. Lifecycle: `READY -> RUNNING -> ZOMBIE -> DEAD`. |
| **Processes** | Full POSIX **fork + execve + wait4**: copy-on-write page-table clone (per-frame refcounts + `VMM_PTE_COW` software bit + COW `#PF` handler with `CR0.WP` enforced), execve replaces caller's address space with a new ELF, wait4 reaps zombies and round-trips the child's `exit_status`. |
| **Userspace** | Ring-3 via `iret`. ELF loader (`elf_exec`) with argc/argv/envp terminator and auxv. Apps: `hello`, `calc`, `sh.elf`, `vix`, `diskinfo`, `fdisk`, `cfdisk`, `basic`, `kbtester`, `makbox`, `clock`, `lines`, `maktop`, `sigtest`, `forktest`, `execvetest`, `alloctest`, `filetest`, `tcc.elf`, and more. |
| **Syscalls** | Linux i386 ABI subset over `int 0x80`: process, file, fd, pipe, signal, time, mmap, TLS, and hosted-libc startup stubs. Makar extensions cover terminal, framebuffer, keyboard, VFS shortcuts, admin, and virtual terminals. `/dev` nodes open as `FD_KIND_BLOCKDEV` so read/write/lseek do sector I/O. |
| **Shell** | `/apps/sh.elf` is the default interactive shell: inline editing, history, PATH lookup, quote-aware tokenization, `sh -c`, pipes, redirection, `&&`/`||`, background `&`, and `wait`. Kernel script layer remains smaller for boot/test scripts. |
| **Drivers** | 16550 UART, PIT, layered PS/2 keyboard (full set-1 + e0 with per-task SPSC rings), ATA/IDE PIO 28-bit LBA, MBR + GPT partition tables. |
| **Debug/Test** | INT 1 / INT 3 GDB-friendly handlers, kernel panic screen, ktest harness, in-guest script drivers, and `kbtest` key injection for keyboard/VT/makmux paths. |
| **Serial** | Linux-style: dmesg + explicit diagnostics by default. `console=ttyS0` cmdline or `verbose [on\|off]` shell builtin opts into TTY-mirroring. |

## Minimum system requirements

Makar targets emulation (QEMU) but is designed to run on period-appropriate
real hardware once installed to disk.

| Resource | Minimum | Recommended |
|---|---|---|
| **CPU** | i686 (Pentium Pro / 1995) — 32-bit protected mode, PSE large pages | Any i686+; runs in QEMU TCG without KVM |
| **RAM** | **32 MiB** (CI/test config boots with `qemu -m 32`; ~29 MiB free after the PMM reserves the kernel) | 256 MiB (kernel identity-maps the low 256 MiB) |
| **Firmware** | BIOS with GRUB Multiboot 2 (the boot device tag is read for auto-mount; `root=/dev/hdaN` on the kernel cmdline picks the rootfs explicitly) | — |
| **Display** | VGA text 80×50 (always works) | Bochs VBE for the 720p VESA framebuffer (`qemu -vga std`) |
| **Storage** | None required to boot (live shell). For persistence: an ATA/IDE disk (PIO, 28-bit LBA) with an MBR or GPT FAT32 partition; optional ext2 partition | ATA HDD with FAT32 boot + ext2 data partitions (auto-mount at `/mnt/boot` + `/mnt/root`; rootfs election lifts the latter to `/`) |
| **Input** | PS/2 keyboard (set-1 + 0xE0 extended) | — |
| **Boot media** | ISO 9660 CD-ROM (`-cdrom makar.iso`) or a FAT32 HDD image with GRUB | — |

USB HID keyboards, networking, and x86-64 are not yet supported (see the
[roadmap](roadmap.md)).

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
| [ext2](kernel/ext2.md) | ext2 filesystem driver (read + write + mkfs) |
| [procfs](kernel/procfs.md) | Synthetic `/proc` filesystem |
| [devfs](kernel/devfs.md) | Synthetic `/dev` block devices (disks, partitions, CD-ROM) |
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
  fs/         fat32.c, ext2.c, iso9660.c, procfs.c, devfs.c, vfs.c
  display/    tty.c, vesa.c + vesa_tty.c, vt.c
  proc/       task.c + task_asm.S, syscall.c, ring3.S, vtty.c
  shell/      shell.c, shell_cmd_*.c
  debug/      exception handlers
src/kernel/kernel/kernel.c    kernel_main
src/kernel/include/kernel/    public headers
src/libc/                     freestanding libc → libk.a
src/userspace/                ring-3 ELF apps
tests/                        gdb_boot_test.py, gdb_hdd_test.py
```

## Acknowledgements

Makar draws on the work of many FOSS projects. Full attribution in
[LICENSES/THANKS.md](https://github.com/Arawn-Davies/makar/blob/main/LICENSES/THANKS.md)
and per-file source-level credits. Key influences:

- **Linux kernel** (GPLv2) — syscall ABI, ELF loading model, process memory layout
- **ELKS / FUZIX** (GPLv2) — minimal-libc / crt0 approach; vi-style editor design
- **CP/M** — terminal-owns-screen philosophy; self-contained program model
- **musl libc** (MIT) — target libc for the userspace porting roadmap
- **GRUB** (GPLv2) — bootloader and Multiboot 2 tag format
- **OSDev wiki** (CC-BY-SA) — cross-compiler setup, paging, descriptor tables
