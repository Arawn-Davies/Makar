/*
 * video.c -- display-driver framework dispatcher (see kernel/video.h).
 *
 * Holds the driver registry and the active binding.  Accelerated drivers are
 * tried first; the dumb LFB driver (video_vbe) is last and always binds, so
 * video_active() is non-NULL after video_init().
 */
#include <kernel/video.h>
#include <kernel/vesa.h>
#include <kernel/serial.h>

extern const vid_driver_t video_vbe;       /* default linear-framebuffer path */
/* Accelerated backends register ahead of video_vbe as they land:
 *   &video_svga2  -- VMware/VirtualBox SVGA II
 *   &video_hyperv -- Hyper-V synthvid (VMBus)
 */

/* Probe order: accelerated backends first, the dumb LFB last (always binds). */
static const vid_driver_t *const s_candidates[] = {
    &video_vbe,
};

static const vid_driver_t *s_active;

void video_init(void)
{
    for (unsigned i = 0; i < sizeof(s_candidates) / sizeof(s_candidates[0]); i++) {
        const vid_driver_t *d = s_candidates[i];
        if (d->probe && !d->probe())
            continue;
        if (d->init && d->init() != 0)
            continue;
        s_active = d;
        Serial_WriteString("video: bound driver ");
        Serial_WriteString((char *)d->name);
        Serial_WriteString(d->caps ? (char *)" (accelerated)\n" : (char *)"\n");
        return;
    }
    /* Unreachable: video_vbe.probe() always returns 1. */
    s_active = &video_vbe;
}

const vid_driver_t *video_active(void) { return s_active; }

uint32_t video_caps(void) { return s_active ? s_active->caps : 0; }

void video_present_rect(const void *src, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h)
{
    if (s_active && s_active->present_rect)
        s_active->present_rect(src, x, y, w, h);
}

void video_present(const void *src)
{
    const vesa_fb_t *fb = vesa_get_fb();
    if (fb)
        video_present_rect(src, 0, 0, fb->width, fb->height);
}
