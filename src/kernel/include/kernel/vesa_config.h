#ifndef _KERNEL_VESA_CONFIG_H
#define _KERNEL_VESA_CONFIG_H

/*
 * VESA framebuffer resolution - the single place to change width/height/bpp.
 *
 * This header is included by both C code (vesa.h) and the Multiboot 2
 * assembly header (boot.s, via -x assembler-with-cpp) so that one edit here
 * is all that is needed to switch between modes such as 640×480×32 or
 * 1024×768×32.
 *
 * Common values:
 *   640  × 480  × 32
 *   800  × 600  × 32
 *   1024 × 768  × 32
 *   1280 × 1024 × 32
 */

/* Default + maximum supported resolution: 720p.  This is the resolution
 * requested from the bootloader via the Multiboot 2 framebuffer tag, so GRUB
 * sets it using the firmware VESA BIOS on EVERY platform -- including ones with
 * no Bochs/DISPI register interface (VMware SVGA, Hyper-V Gen1).  GRUB falls
 * back to the closest available mode if 720p isn't offered.  QEMU/Bochs are
 * also held at 720p (the kernel's DISPI upgrade is capped to match); 720p is a
 * good ceiling for a software compositor (1080p is ~2x the pixels to push). */
#define VESA_WIDTH   1280
#define VESA_HEIGHT  720
#define VESA_BPP 32

#endif /* _KERNEL_VESA_CONFIG_H */
