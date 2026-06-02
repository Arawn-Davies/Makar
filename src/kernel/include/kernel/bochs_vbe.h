#ifndef KERNEL_BOCHS_VBE_H
#define KERNEL_BOCHS_VBE_H

#include <stdbool.h>
#include <stdint.h>

/* Bochs VBE / QEMU BGA interface - accessible via I/O ports from protected
 * mode without requiring real-mode BIOS calls. */

/* Returns true if a Bochs-compatible VBE adapter is detected. */
bool bochs_vbe_available(void);

/* Total video memory in bytes, read from the Bochs DISPI VIDEO_MEMORY_64K
 * register.  Returns 0 if the adapter doesn't report it. */
uint32_t bochs_vbe_vram_bytes(void);

/* Maximum mode the adapter advertises via the DISPI GETCAPS protocol.
 * Any of the out-params may come back 0 if the adapter doesn't report a
 * given cap. */
void bochs_vbe_caps(uint32_t *max_w, uint32_t *max_h, uint32_t *max_bpp);

/* True if width×height×bpp is within both the advertised dimension caps
 * and the available VRAM.  Returns false when no VBE adapter is present.
 * Used to pick the boot resolution and to gate `setmode` so we never
 * program a mode the adapter can't scan out (black screen / FB overrun). */
bool bochs_vbe_mode_supported(uint32_t width, uint32_t height, uint32_t bpp);

/* Switch the linear framebuffer to width×height at bpp bits per pixel.
 * LFB_ENABLED is set so the framebuffer address stays constant.
 * Call vesa_update_geometry() + vesa_tty_init() after this. */
void bochs_vbe_set_mode(uint32_t width, uint32_t height, uint8_t bpp);

/* Disable VBE graphics.  QEMU reverts to VGA text mode (mode 3, 80×25)
 * and the canonical 8×16 IBM VGA ROM font is uploaded to plane 2. */
void bochs_vbe_disable(void);

/* Replace the plane-2 font with the kernel's 8×8 FONT8x8 glyphs (8 bytes
 * per char, the rest of each 32-byte slot zeroed).  Call this after
 * switching to 80×50, where the CRTC reads only the first 8 bytes of
 * each glyph - the top half of an 8×16 glyph clips the letter body. */
void vga_load_text_font_8x8(void);


#endif /* KERNEL_BOCHS_VBE_H */
