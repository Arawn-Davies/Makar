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

/* Uncomment VESA_AUTO to request the highest common resolution (1920×1080)
 * from the bootloader and let GRUB fall back to the best available mode.
 * Leave commented to use the fixed 640×480 safe default. */
/* #define VESA_AUTO */

#ifdef VESA_AUTO
# define VESA_WIDTH  1920
# define VESA_HEIGHT 1080
#else
# define VESA_WIDTH   640
# define VESA_HEIGHT  480
#endif
#define VESA_BPP 32

#endif /* _KERNEL_VESA_CONFIG_H */
