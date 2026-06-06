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
 * requested from the bootloader via the Multiboot 2 framebuffer tag.
 *
 * We ask for 1024x768x32 rather than 720p because that's what the no-DISPI
 * platforms can actually deliver: GRUB sets this via the firmware VESA BIOS on
 * Hyper-V Gen1 / VMware SVGA (no Bochs/DISPI register interface).  Hyper-V
 * Gen1's pre-OS VBE has NO 16:9 modes and won't honour gfxpayload for a
 * multiboot2 kernel -- its mode list tops out at 1024x768x32 / 1152x864x32
 * (4:3 / 5:4), so 1280x720 was unreachable and it fell back to the EDID-
 * preferred 800x600.  1024x768x32 is in every adapter's list, so it's the
 * reliable request.  QEMU/Bochs ignore this -- the kernel's DISPI path upgrades
 * them to 720p (see kernel.c) -- so the dev/test resolution is unchanged.
 * Going higher on Hyper-V (1280x1024 only exists at 16bpp there) needs a
 * synthetic-video driver; see the roadmap. */
#define VESA_WIDTH   1024
#define VESA_HEIGHT  768
#define VESA_BPP 32

#endif /* _KERNEL_VESA_CONFIG_H */
