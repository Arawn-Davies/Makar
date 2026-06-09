#ifndef _KERNEL_VESA_H
#define _KERNEL_VESA_H

#include <stdint.h>
#include <stdbool.h>
#include <kernel/multiboot.h>
#include <kernel/vesa_config.h>

/* Preferred framebuffer geometry (requested via the Multiboot 2 header tag).
 * These resolve to the values in <kernel/vesa_config.h>. */
#define VESA_PREFERRED_WIDTH   VESA_WIDTH
#define VESA_PREFERRED_HEIGHT  VESA_HEIGHT
#define VESA_PREFERRED_BPP     VESA_BPP

/* Framebuffer descriptor populated by vesa_init(). */
typedef struct
{
	uint32_t *addr;    /* mapped base address of the framebuffer */
	uint32_t  pitch;   /* bytes per scanline */
	uint32_t  width;   /* width in pixels */
	uint32_t  height;  /* height in pixels */
	uint8_t   bpp;     /* bits per pixel */
	/* RGB channel layout */
	uint8_t   red_shift;
	uint8_t   red_bits;
	uint8_t   green_shift;
	uint8_t   green_bits;
	uint8_t   blue_shift;
	uint8_t   blue_bits;
} vesa_fb_t;

/*
 * Locate the Multiboot 2 framebuffer tag and, if found, populate the global
 * framebuffer descriptor.  Returns true on success, false if no framebuffer
 * tag was found or the framebuffer type is not direct RGB.
 */
bool vesa_init(multiboot2_info_t *mbi);

/* Return a pointer to the global framebuffer descriptor, or NULL if
 * vesa_init() has not been called successfully. */
const vesa_fb_t *vesa_get_fb(void);

/* Write a 32-bit ARGB colour to pixel (x, y).
 * The caller is responsible for ensuring x < fb->width and y < fb->height;
 * out-of-bounds coordinates will corrupt adjacent memory or crash the system. */
void vesa_put_pixel(uint32_t x, uint32_t y, uint32_t colour);

/* Fill the entire framebuffer with a single 32-bit ARGB colour. */
void vesa_clear(uint32_t colour);

/* Disable the framebuffer (marks fb_ready = false).
 * Call before switching to VGA text mode so vesa_tty output stops. */
void vesa_disable(void);

/* Update the framebuffer geometry after a Bochs VBE mode change.
 * Sets fb_ready = true; the base address is unchanged (LFB stays constant). */
void vesa_update_geometry(uint32_t width, uint32_t height, uint8_t bpp);

/* Repoint the framebuffer base + stride + geometry wholesale.  Used by a display
 * driver that mode-sets and reports its own FB (e.g. SVGA II). */
void vesa_set_framebuffer(uint32_t *addr, uint32_t pitch, uint32_t width,
                          uint32_t height, uint8_t bpp);

/* Blit the splash logo (from logo.h) centred on the framebuffer.
 * fg and bg are 24-bit RGB colours for foreground and background pixels. */
void vesa_blit_logo(uint32_t fg, uint32_t bg);

/* Graphical boot splash: bg fill + the colour disc emblem + the MAKAR wordmark
 * (fg), centred, with an empty loading-bar frame, flushed through the active
 * display driver. */
void vesa_draw_splash(uint32_t fg, uint32_t bg);

/* Fill the splash loading bar to done/total.  No-op before vesa_draw_splash. */
void vesa_splash_progress(int done, int total);

#endif
