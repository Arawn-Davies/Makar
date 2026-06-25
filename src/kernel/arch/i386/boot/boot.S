#include <kernel/vesa_config.h>

# Multiboot 2 header constants.
# MB2_HEADER_LEN is hardcoded (48) because TCC's assembler cannot resolve
# forward symbol references; computing (mb2_header_end - mb2_header_start)
# segfaults TCC.  boot/mb2_header_check.c carries a struct + _Static_assert
# that fails the build if the byte layout below ever drifts from 48.
.set MB2_MAGIC,        0xE85250D6  # Multiboot 2 magic
.set MB2_ARCH,         0           # i386 protected mode
.set MB2_HEADER_LEN,   48
.set MB2_CHECKSUM,     -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

# Higher-half kernel virtual base.  Must match KERNEL_VBASE in linker.ld.
.set KERNEL_VBASE,     0xC0000000
# Page directory index of KERNEL_VBASE (0xC0000000 >> 22 = 768).
.set KERNEL_PD_IDX,    (KERNEL_VBASE >> 22)

# 4 MiB-page PDE flags: present | writable | page-size (PSE large page).
.set PDE_PRESENT_RW_LARGE, 0x83

# Declare a Multiboot 2 header.  Must be 8-byte aligned and within the
# first 32768 bytes of the binary image.
# Under gcc, the linker script places *(.multiboot2) at the front of .boot.
# TCC's linker has no script support, so under TCC we drop the header into
# .text directly and rely on boot.o being linked first.
#ifdef __TINYC__
.section .text
#else
.section .multiboot2
#endif
.align 8
mb2_header_start:
.long  MB2_MAGIC
.long  MB2_ARCH
.long  MB2_HEADER_LEN
.long  MB2_CHECKSUM
# Framebuffer tag (type=5): request a linear framebuffer from the bootloader.
# flags=1 means the tag is optional – the kernel still boots in text mode if
# the bootloader cannot honour the request.
.align 8
.short 5        # type = framebuffer request
.short 1        # flags = optional
.long  20       # size  = 20 bytes (header + 3 × uint32)
.long  VESA_WIDTH    # preferred width  - change in kernel/include/kernel/vesa_config.h
.long  VESA_HEIGHT   # preferred height
.long  VESA_BPP      # preferred depth (bits per pixel)
# End tag (type=0, flags=0, size=8)
.align 8
.short 0
.short 0
.long  8
mb2_header_end:

# Reserve a stack for the initial thread.  Linked high (.bss); only used once
# paging is on.  A page-aligned guard page sits directly below the stack:
# paging_protect_kernel() unmaps it in the high window, so a kernel-stack
# overflow (ESP runs below stack_bottom) takes a clean #PF instead of silently
# corrupting whatever .bss lies beyond -- the kernel-side analogue of the
# unmapped page below the ring-3 stack.
.section .bss
.align 4096
.global kstack_guard
kstack_guard:
.skip 4096                  # guard page (unmapped after paging is up)
.global stack_bottom
stack_bottom:
.skip 16384 # 16 KiB
stack_top:

#ifndef __TINYC__
# Static boot page directory, 4 KiB aligned.  Linked high (.bss) so it is
# zeroed by the Multiboot loader; the boot stub fills the few entries it needs
# using its PHYSICAL address (virtual - KERNEL_VBASE), since paging is off.
.align 4096
.global boot_page_directory
boot_page_directory:
.skip 4096
#endif

#ifdef __TINYC__
# ---------------------------------------------------------------------------
# TCC build: no linker script, kernel is linked + runs low-half (identity).
# Paging stays off here and is enabled later by paging_init().  This is the
# original, pre-higher-half entry path.
# ---------------------------------------------------------------------------
.section .text
.global _start
.type _start, @function
_start:
	movl $stack_top, %esp

	pushl %eax
	pushl %ebx
	call _init
	popl %ebx
	popl %eax
	pushl %ebx
	pushl %eax
	call kernel_main

	cli
1:	hlt
	jmp 1b
.size _start, . - _start

#else
# ---------------------------------------------------------------------------
# GCC higher-half build.
#
# Boot stub.  Linked + loaded at physical 1 MiB and runs with paging OFF, so
# it lives in .boot where virtual address == physical address.  It builds a
# minimal page directory (identity-maps the low 16 MiB so EIP/ESP/MBI stay
# valid the instant paging turns on, and maps 0xC0000000.. -> 0x0.. so the
# high kernel image becomes reachable), enables paging, then far-jumps into
# the high virtual image.
# ---------------------------------------------------------------------------
.section .boot, "ax"
.global _start
.type _start, @function
_start:
	cli

	# Stash the Multiboot2 magic (EAX) in ESI before the boot stub uses EAX as
	# scratch for the PDE flags / CR4 / CR0 below.  ESI is untouched through the
	# stub and the far jump, so higher_half_entry can recover the magic from it.
	# (EBX = MBI pointer is never clobbered by the stub, so it needs no save.)
	movl %eax, %esi

	# Physical address of the boot page directory.
	movl $(boot_page_directory - KERNEL_VBASE), %edi

	# Identity-map the first 16 MiB: PDE[0..3] -> 0,4,8,12 MiB (4 MiB large
	# pages).  This covers the boot stub, MBI, GRUB modules and low data so
	# the instruction stream and stack survive the moment paging turns on.
	movl $PDE_PRESENT_RW_LARGE, %eax        # phys 0 | flags
	movl $4, %ecx
	movl %edi, %ebp
1:
	movl %eax, (%ebp)
	addl $0x00400000, %eax                  # next 4 MiB physical frame
	addl $4, %ebp
	loop 1b

	# High-half map: PDE[768..771] (0xC0000000..0xC1000000) -> 0..16 MiB.
	# Four large pages give the kernel image 16 MiB of headroom (image +
	# static BSS such as mem_map[] and the boot PD).
	movl $PDE_PRESENT_RW_LARGE, %eax        # phys 0 | flags
	leal (KERNEL_PD_IDX * 4)(%edi), %ebp    # &boot_page_directory[768] (phys)
	movl $4, %ecx
2:
	movl %eax, (%ebp)
	addl $0x00400000, %eax
	addl $4, %ebp
	loop 2b

	# Load CR3 with the physical address of the boot page directory.
	movl %edi, %cr3

	# Enable CR4.PSE so the PS bit in the PDEs selects 4 MiB large pages.
	movl %cr4, %eax
	orl  $0x00000010, %eax                  # CR4.PSE
	movl %eax, %cr4

	# Enable paging (CR0.PG) and write-protect (CR0.WP, bit 16).
	movl %cr0, %eax
	orl  $0x80010000, %eax                  # PG | WP
	movl %eax, %cr0

	# Paging is on.  EIP is still executing from the identity-mapped low copy.
	# Jump into the high virtual image to load EIP with the high label.  Use
	# ECX as the scratch register -- NOT EAX (holds the Multiboot2 magic) or
	# EBX (holds the MBI pointer), which higher_half_entry must still save.
	leal higher_half_entry, %ecx
	jmp  *%ecx

# ---------------------------------------------------------------------------
# From here on we run at high virtual addresses.  This code lives in .text
# (linked high), so the label resolves to a 0xC01xxxxx address.
# ---------------------------------------------------------------------------
.section .text
.type higher_half_entry, @function
higher_half_entry:
	# Switch to the high virtual stack.
	movl $stack_top, %esp

	# The low identity map (PDE[0..3]) is left in place: the kernel keeps a
	# permanent identity window for the low physical region (PMM frames,
	# Multiboot info, VGA buffer, ACPI tables).  paging_init() later installs
	# the full runtime page directory (256 MiB identity + 256 MiB high).

	# Save the multiboot2 magic (stashed in ESI by _start, since the boot stub
	# clobbered EAX) and the MBI pointer (EBX) before calling _init, which may
	# clobber caller-saved registers.  EBX is a PHYSICAL pointer into low
	# memory; it stays valid via the identity map.
	pushl %esi
	pushl %ebx

	# Call the global constructors.
	call _init

	# Restore and pass multiboot2 info to kernel_main.
	# Push mbi pointer (ebx) then magic (eax) so kernel_main receives them
	# as (uint32_t magic, multiboot2_info_t *mbi) per i386 cdecl.
	popl %ebx
	popl %eax
	pushl %ebx
	pushl %eax
	call kernel_main

	# Hang if kernel_main unexpectedly returns.
	cli
1:	hlt
	jmp 1b
.size higher_half_entry, . - higher_half_entry

#endif /* __TINYC__ */
