---
title: Glossary
layout: default
nav_order: 20
permalink: /glossary
---

# Glossary

Abbreviations and jargon used across the Makar source, docs, and commit log.
Grouped by area. If you hit a term in the tree that isn't here, it's a bug in
this page — open an issue.

## Boot & firmware

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **BIOS** | Basic Input/Output System | Legacy 16-bit PC firmware. Makar boots BIOS/CSM, not UEFI. |
| **GRUB** | GRand Unified Bootloader | The bootloader on the build ISO; loads the kernel via Multiboot 2. |
| **Limine** | *(proper noun)* | The bootloader the Makar installer writes to an installed HDD (BIOS/MBR). |
| **Multiboot 2** | — | The GRUB/Limine ↔ kernel handoff spec: a header in the kernel image requests services (e.g. a framebuffer), the loader passes back an info structure (the **MBI**). |
| **MBI** | Multiboot Information | The tag list the bootloader hands the kernel (memory map, framebuffer, cmdline…). |
| **MBR** | Master Boot Record | First 512 bytes of a disk: boot code + partition table. |
| **GPT** | GUID Partition Table | Modern partition scheme; Makar can read it. |
| **gfxpayload** | graphics payload *(GRUB var)* | The video mode GRUB sets before handing off. A comma list lets it pick the best mode the firmware actually offers. |
| **LFB** | Linear Frame Buffer | A flat, memory-mapped pixel buffer in a graphics mode (vs. the legacy VGA text buffer). |

## Display

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **VGA** | Video Graphics Array | The legacy display standard; here usually the 80×50 text fallback at `0xB8000`. |
| **VESA** | Video Electronics Standards Association | The standard behind high-res linear framebuffers; Makar's `vesa.c` drives one. |
| **VBE** | VESA BIOS Extensions | Firmware interface for setting VESA modes. What Hyper-V Gen1 / VMware expose (and what limits Hyper-V to 4:3 modes — no 16:9). |
| **DISPI** | Bochs Display Interface | A simple register interface (QEMU/Bochs) for changing resolution *after* boot, without the VBE BIOS. Hyper-V/VMware lack it. |
| **bpp** | bits per pixel | Colour depth; Makar uses 32 (XRGB). |
| **pitch** | — | Bytes per scanline in the framebuffer (may exceed `width × bpp/8` due to padding). |
| **WM** | Window Manager | The userspace GUI server (`gui.elf` / `wm.c`); sole compositor. |
| **compositor** | — | The component that blits all window buffers into the one framebuffer. |
| **DPI / fps** | dots per inch / frames per second | Pixel density / refresh rate. |
| **ANSI / VT100** | — | The escape-sequence terminal standard (cursor moves, colours, scrolling). `mxterm` emulates it via the `vt100.c` core so TUI apps render in a GUI window. |
| **CSI** | Control Sequence Introducer | The `ESC [` prefix of most ANSI sequences (e.g. `ESC[31m`, `ESC[2J`). |
| **SGR** | Select Graphic Rendition | The `ESC[…m` family that sets colour/bold/reverse for following text. |
| **CUP / ED / EL** | Cursor Position / Erase Display / Erase Line | Common CSI ops: move the cursor, clear the screen, clear a line. |
| **pty** | pseudo-terminal | A terminal not backed by hardware — here the GUI terminal's pipe to its shell. `SYS_PTY_WINSIZE` publishes its size. |
| **TUI** | Text User Interface | A full-screen text-mode app drawn with the cell API (maktop, vix, cfdisk). |

## Memory & CPU

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **PMM** | Physical Memory Manager | The frame allocator (bitmap of physical pages). |
| **VMM** | Virtual Memory Manager | Per-task page-directory management. |
| **PD / PT / PDE / PTE** | Page Directory / Table / Directory Entry / Table Entry | The two-level i386 paging structures and their entries. |
| **TLB** | Translation Lookaside Buffer | The CPU's cache of virtual→physical translations; flushed by reloading **CR3**. |
| **PSE** | Page Size Extensions | Enables 4 MiB "large" pages (CR4.PSE). |
| **COW** | Copy-On-Write | Shared read-only pages duplicated lazily on first write (used by `fork`). |
| **CR0/CR3/CR4** | Control Registers | CR0 = paging/cache bits, CR3 = page-directory base, CR4 = feature flags (PSE…). |
| **WP** | Write Protect (CR0 bit) | Makes ring-0 honour read-only user PTEs — required for COW. |
| **MSR** | Model-Specific Register | CPU config registers read/written with `rdmsr`/`wrmsr`. |
| **PAT** | Page Attribute Table | An MSR that lets a **PTE** pick a memory (cache) type per page, without touching MTRRs. Makar arms a Write-Combining slot for the framebuffer. |
| **MTRR** | Memory Type Range Register | MSRs that assign a cache type to a physical address *range*. Firmware marks the PCI MMIO hole **UC**; we add a **WC** range over the framebuffer so pixel writes batch. |
| **Cache types** | UC / UC- / WC / WT / WP / WB | Uncacheable / UC-minus / Write-Combining / Write-Through / Write-Protect / Write-Back. UC traps every write (slow); WC batches small writes into bursts (fast for a framebuffer). |
| **wbinvd** | Write Back and Invalidate Cache | Instruction that flushes the CPU caches; required around MTRR changes. |
| **FPU / SSE / FXSAVE** | Floating-Point Unit / Streaming SIMD Extensions | x87+SSE state, saved per task via the `fxsave`/`fxrstor` instructions. |
| **TLS** | Thread-Local Storage | Per-task storage; i386 reaches it via a GDT segment (`%gs`). |
| **GDT / IDT** | Global Descriptor Table / Interrupt Descriptor Table | The segment and interrupt-vector tables. |
| **PIT** | Programmable Interval Timer | The 8253/8254 timer; Makar runs it at **TIMER_HZ** (250 Hz) to drive preemption. |
| **TIMER_HZ / quantum** | — | Internal tick rate (250 Hz = 4 ms) and the per-task time-slice (`g_sched_quantum × sched_weight` ticks) before the scheduler preempts. |
| **USER_HZ** | — | The *userspace-facing* tick rate, fixed at 100 Hz regardless of TIMER_HZ (like Linux). `SYS_UPTIME` and `/proc` CPU ticks report in these units so apps' timing stays correct. |

## Devices, power & virtualization

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **ACPI** | Advanced Configuration and Power Interface | Firmware tables + bytecode for power management; Makar uses it for soft-off and reboot. |
| **AML** | ACPI Machine Language | The bytecode in the DSDT; Makar decodes just the `\_S5_` package. |
| **RSDP / RSDT / XSDT** | Root System Description Pointer / Table / eXtended SDT | The ACPI table roots. XSDT holds 64-bit pointers (preferred on ACPI 2.0+). |
| **FADT** | Fixed ACPI Description Table | Points at the DSDT and holds the power-management ports (PM1, SMI_CMD…). |
| **DSDT** | Differentiated System Description Table | The main AML table; contains `\_S5_` (soft-off sleep values). |
| **S5** | Sleep state 5 | "Soft off" — the ACPI power-off state. |
| **PM1a/PM1b_CNT** | Power Management 1 Control | The I/O ports a sleep value (`SLP_TYPx | SLP_EN`) is written to, to power off. |
| **SCI / SCI_EN** | System Control Interrupt / SCI Enable | SCI_EN (PM1 bit 0) = "ACPI mode is on". Hyper-V Gen1 boots with it **off**; we run the enable handshake first. |
| **SMI / SMI_CMD** | System Management Interrupt / SMI Command port | Writing `ACPI_ENABLE` to SMI_CMD asks firmware to switch into ACPI mode. |
| **PS/2** | Personal System/2 | The legacy keyboard/mouse port interface. |
| **PIT** | Programmable Interval Timer | The 250 Hz timer that drives preemption (TIMER_HZ). |
| **PCI** | Peripheral Component Interconnect | The bus; its **MMIO hole** is the high physical region devices (incl. the framebuffer) live in. |
| **MMIO** | Memory-Mapped I/O | Device registers/memory accessed as if they were RAM. |
| **IDE / ATA / ATAPI** | Integrated Drive Electronics / AT Attachment / ATA Packet Interface | The disk (and CD-ROM, ATAPI) interface; Makar uses PIO and DMA. |
| **DMA / PIO** | Direct Memory Access / Programmed I/O | Disk transfer via a bus-master engine (fast) vs. CPU-copied through a port (simple). |
| **CPUID** | CPU Identification | The instruction that reports CPU features and the hypervisor signature. |
| **KVM** | Kernel-based Virtual Machine | Linux's hardware-accelerated virtualization; QEMU uses it when present. |
| **TCG** | Tiny Code Generator | QEMU's pure-software (no-KVM) CPU emulation; the correctness gates run under it. |
| **VT-x** | Intel Virtualization Technology | Hardware virtualization extensions; under them (VMware/Hyper-V/VirtualBox/KVM) UC framebuffer writes are genuinely slow, hence the WC work. |

## Filesystems & processes

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **VFS** | Virtual File System | The mount table that routes paths to ext2/FAT32/ISO9660/synthetic backends. |
| **ext2 / FAT32 / ISO9660** | — | The on-disk/optical filesystems Makar reads and (for ext2/FAT32) writes. |
| **ELF** | Executable and Linkable Format | The binary format of ring-3 apps (`*.elf`). |
| **PID / fd** | Process ID / file descriptor | Standard POSIX process and open-file handles. |
| **IPC** | Inter-Process Communication | Message passing; the GUI uses the **makx** protocol (MINIX-style synchronous messages) plus shared surfaces. |
| **ABI** | Application Binary Interface | The binary contract (syscall numbers, struct layouts) between kernel and userspace. |
| **libc / libk** | C library (hosted / freestanding kernel) | `libc.a` for ring-3 apps; `libk.a` linked into the kernel. |
| **TUI / GUI** | Text / Graphical User Interface | Cell-grid terminal apps vs. windowed pixel apps. |

## Toolchain

| Term | Expansion | What it means here |
|------|-----------|--------------------|
| **GCC / Binutils** | GNU Compiler Collection / GNU binary utilities | The cross toolchain (`i686-elf-*`, future `i686-makar-*`). |
| **musl / newlib** | — | Candidate hosted C libraries for a future userspace port. |
| **TinyCC (tcc)** | Tiny C Compiler | The in-OS C compiler shipped as `tcc.elf`. |
| **ccache** | compiler cache | Speeds rebuilds; toggle with `CCACHE=0`. |
| **sysroot** | system root | The directory tree a cross-compiler treats as `/` for headers/libs. |
