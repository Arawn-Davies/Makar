/*
 * video_vbe.c -- the default display driver: a dumb linear framebuffer.
 *
 * present_rect is the per-line CPU memcpy into the LFB that SYS_FB_PRESENT used
 * to do inline; the cursor is software-composited by the window manager (this
 * driver advertises no caps, so the WM keeps its save-under cursor path).  This
 * is the universal fallback -- QEMU-std, Bochs/DISPI, VirtualBox VGA, bare metal
 * -- and the one the accelerated drivers degrade to when their hardware is
 * absent or bring-up fails.
 */
#include <kernel/video.h>
#include <kernel/vesa.h>
#include <string.h>

static int vbe_probe(void) { return 1; }   /* always available */

static int vbe_init(void)
{
    /* vesa_init() already mapped the LFB and set the geometry; nothing to do. */
    return vesa_get_fb() ? 0 : -1;
}

static void vbe_present_rect(const void *src, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h)
{
    const vesa_fb_t *fb = vesa_get_fb();
    if (!fb || !src)
        return;
    if (x >= fb->width || y >= fb->height)
        return;
    if (x + w > fb->width)  w = fb->width  - x;
    if (y + h > fb->height) h = fb->height - y;

    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)fb->addr;
    uint32_t src_pitch  = fb->width * 4u;     /* back buffer is full-frame packed */
    uint32_t copy_bytes = w * 4u;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t line = y + row;
        memcpy(d + line * fb->pitch + x * 4u,
               s + line * src_pitch + x * 4u, copy_bytes);
    }
}

const vid_driver_t video_vbe = {
    .name         = "vbe-lfb",
    .probe        = vbe_probe,
    .init         = vbe_init,
    .present_rect = vbe_present_rect,
    .cursor_define = 0,
    .cursor_move   = 0,
    .cursor_show   = 0,
    .caps         = 0,
};
