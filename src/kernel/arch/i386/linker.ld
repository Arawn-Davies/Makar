/* The bootloader will look at this image and start execution at the symbol
   designated at the entry point. */
ENTRY(_start)

/* Higher-half kernel.
 *
 * The kernel is *loaded* by GRUB at physical 1 MiB (the conventional spot) but
 * *linked* to run at virtual 0xC0100000 (KERNEL_VBASE + 1 MiB).  The boot stub
 * in boot.S runs at its physical address with paging off, builds a page
 * directory mapping the kernel both identity (low) and at 0xC0000000, turns
 * paging on, then far-jumps into the high virtual image.
 *
 * The .boot section (early entry + the static boot page tables) is the only
 * code that runs with paging off, so it is linked at its physical load address
 * (no virtual offset).  Everything else is linked high and placed at a low
 * physical LMA via AT(ADDR(.section) - KERNEL_VBASE).
 */
KERNEL_VBASE = 0xC0000000;

SECTIONS
{
	/* The boot stub: linked + loaded at physical 1 MiB.  Runs with paging
	   off, so its virtual address must equal its physical address.  The
	   Multiboot 2 header must live very early in the image, so it goes here
	   too.  _start (the Multiboot entry) therefore has a low (physical)
	   address, which is what the bootloader jumps to and what the GDB boot
	   test breaks on while paging is still off. */
	. = 1M;

	_kernel_phys_start = .;

	.boot BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot2)
		*(.boot)
	}

	/* The rest of the kernel is linked high (KERNEL_VBASE + ...) but loaded
	   immediately after the boot stub in physical memory. */
	. += KERNEL_VBASE;

	.text BLOCK(4K) : AT(ADDR(.text) - KERNEL_VBASE) ALIGN(4K)
	{
		*(.text)
	}

	/* Read-only data. */
	.rodata BLOCK(4K) : AT(ADDR(.rodata) - KERNEL_VBASE) ALIGN(4K)
	{
		*(.rodata)
	}

	/* Read-write data (initialized) */
	.data BLOCK(4K) : AT(ADDR(.data) - KERNEL_VBASE) ALIGN(4K)
	{
		*(.data)
	}

	/* Read-write data (uninitialized) and stack */
	.bss BLOCK(4K) : AT(ADDR(.bss) - KERNEL_VBASE) ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}

	/* Symbol marking the end of the kernel image.  Used by the PMM to
	   determine which physical frames are occupied by the kernel.  This is a
	   *virtual* (high) address; the PMM subtracts KERNEL_VBASE to get the
	   physical end. */
	_kernel_end = .;

	/* Physical end of the loaded kernel image, for the PMM reservation. */
	_kernel_phys_end = . - KERNEL_VBASE;

	/* The compiler may produce other sections, put them in the proper place in
	   in this file, if you'd like to include them in the final kernel. */
}
