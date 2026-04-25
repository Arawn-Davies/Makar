#include <kernel/vesa_config.h>

# Multiboot 2 header constants.
.set MB2_MAGIC,        0xE85250D6  # Multiboot 2 magic
.set MB2_ARCH,         0           # i386 protected mode
.set MB2_HEADER_LEN,   (mb2_header_end - mb2_header_start)
.set MB2_CHECKSUM,     -(MB2_MAGIC + MB2_ARCH + MB2_HEADER_LEN)

# Declare a Multiboot 2 header.  Must be 8-byte aligned and within the
# first 32768 bytes of the binary image.
.section .multiboot2
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
.long  VESA_WIDTH    # preferred width  — change in kernel/include/kernel/vesa_config.h
.long  VESA_HEIGHT   # preferred height
.long  VESA_BPP      # preferred depth (bits per pixel)
# End tag (type=0, flags=0, size=8)
.align 8
.short 0
.short 0
.long  8
mb2_header_end:

# Reserve a stack for the initial thread.
.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KiB
stack_top:

# The kernel entry point.
.section .text
.global _start
.type _start, @function
_start:
	movl $stack_top, %esp

	# Save the MBI pointer (ebx) and multiboot2 magic (eax) before calling
	# _init, which may clobber caller-saved registers including eax.
	pushl %eax
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
.size _start, . - _start
