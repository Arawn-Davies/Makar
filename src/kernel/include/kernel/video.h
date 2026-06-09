#ifndef _KERNEL_VIDEO_H
#define _KERNEL_VIDEO_H

#include <stdint.h>

/*
 * Display-driver framework.
 *
 * The kernel owns one active display driver behind a small vtable.  The default
 * driver (video_vbe) is the dumb linear-framebuffer path: present = a per-line
 * memcpy into the LFB and a software-composited cursor, which covers QEMU-std,
 * Bochs/DISPI, VirtualBox VGA and bare metal.  Accelerated drivers (VMware/
 * VirtualBox SVGA II, Hyper-V synthvid) bind ahead of it when their hardware is
 * present, advertising 2D copy and/or a hardware cursor through `caps`.
 *
 * Everything that used to touch the LFB directly (SYS_FB_PRESENT[_RECT], the WM
 * cursor) now routes through video_active(), so a driver can intercept the blit
 * (e.g. issue a 2D UPDATE rather than a CPU copy) and own the cursor.  All
 * drivers share the framebuffer geometry from vesa_get_fb(); the back buffer a
 * client presents is full-frame, tightly packed 32-bpp (pitch = width*4).
 */

#define VID_CAP_ACCEL_COPY  0x1u   /* present_rect is a 2D op, not a CPU memcpy */
#define VID_CAP_HW_CURSOR   0x2u   /* cursor_* drive a real hardware sprite     */

typedef struct vid_driver {
    const char *name;

    /* Return non-zero if this driver should bind on this machine.  Called in
     * registry order; the first match wins, else the default LFB driver. */
    int  (*probe)(void);

    /* Bring the device up to the current vesa_get_fb() geometry.  Return 0 on
     * success; non-zero makes video_init fall back to the next candidate. */
    int  (*init)(void);

    /* Switch to (w,h)x32 at runtime via the device's own registers and repoint
     * the framebuffer (vesa_set_framebuffer).  Return 0 on success, non-zero if
     * unsupported.  NULL on backends that can't mode-set (plain VBE LFB), so the
     * caller falls back to the Bochs DISPI path. */
    int  (*set_mode)(uint32_t w, uint32_t h);

    /* Present the rectangle (x,y,w,h) of a full-frame packed-32bpp back buffer
     * `src` (pitch = fb->width*4) to the screen. */
    void (*present_rect)(const void *src, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h);

    /* Scan out a rectangle the caller has already written directly into the
     * framebuffer (no copy).  NULL for backends whose framebuffer is live (plain
     * VBE LFB); SVGA II needs it because direct writes aren't visible until an
     * explicit UPDATE.  The text console paints the FB directly, so it flushes
     * through here. */
    void (*flush_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    /* Hardware cursor (only meaningful with VID_CAP_HW_CURSOR).  define takes a
     * w*h ARGB sprite; move positions its hotspot; show toggles visibility. */
    int  (*cursor_define)(const uint32_t *argb, int w, int h,
                          int hot_x, int hot_y);
    void (*cursor_move)(int x, int y);
    void (*cursor_show)(int on);

    uint32_t caps;
} vid_driver_t;

/* Walk the driver registry, bind the first whose probe() (then init()) passes,
 * else the default LFB driver.  Call once after vesa_init(). */
void video_init(void);

/* The bound driver (never NULL after video_init -- the LFB driver always binds). */
const vid_driver_t *video_active(void);

/* Capability bits of the active driver (0 before video_init). */
uint32_t video_caps(void);

/* Present helpers used by SYS_FB_PRESENT[_RECT]: route to the active driver. */
void video_present(const void *src);                       /* full frame */
void video_present_rect(const void *src, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);

/* Scan out a rectangle already written directly into the framebuffer (text
 * console path).  No-op on backends with a live framebuffer. */
void video_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Ask the active driver to switch to (w,h)x32 via its own registers (SVGA II).
 * Returns 0 on success, non-zero if the driver can't do it (caller falls back
 * to Bochs DISPI). */
int video_set_mode(uint32_t w, uint32_t h);

/* Panic path: disable the SVGA II engine so the device reverts to VGA-
 * compatible scanout (the panic then programs VGA mode 3 + writes 0xB8000).
 * No-op if the SVGA II driver never bound.  Defined in video_svga2.c. */
void video_svga2_to_vga(void);

#endif /* _KERNEL_VIDEO_H */
