/*
 * gui_gfx.h -- tiny 32-bpp software drawing layer for the Makar GUI.
 *
 * Everything draws into a gfx_surface (a tightly-packed XRGB8888 pixel buffer,
 * row-major, pitch == w).  The window manager's back buffer is one such
 * surface; each window composites into it.  All ops clip to the surface
 * bounds, so callers can draw freely without bounds-checking.
 *
 * Freestanding userspace: no libc, no stdint (unsigned int is 32-bit on i686).
 */
#ifndef GUI_GFX_H
#define GUI_GFX_H

typedef unsigned int  gfx_u32;
typedef unsigned char gfx_u8;

typedef struct {
    gfx_u32 *px;        /* w*h pixels, row-major                            */
    int      w, h;
} gfx_surface;

#define GFX_RGB(r,g,b) (((gfx_u32)(r)<<16)|((gfx_u32)(g)<<8)|(gfx_u32)(b))

/* One pixel (clipped). */
void gfx_px(gfx_surface *s, int x, int y, gfx_u32 c);

/* Solid rectangle (clipped). */
void gfx_fill(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c);

/* 1px rectangle outline. */
void gfx_outline(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c);

/* Filled rectangle with the four corner pixels knocked out to `bg` -> a cheap
 * rounded look (matches the dock pills in the old wm.c). */
void gfx_round(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c, gfx_u32 bg);

/* 8x8 glyph / string from font8x8.h.  `ch >= 128` renders as '?'. */
void gfx_char(gfx_surface *s, int x, int y, unsigned char ch, gfx_u32 fg);
void gfx_str(gfx_surface *s, int x, int y, const char *str, gfx_u32 fg);
/* Like gfx_str but stops before column x_max (pixels) — keeps text in a rect. */
void gfx_str_clip(gfx_surface *s, int x, int y, const char *str, gfx_u32 fg, int x_max);

/* Pixel width of a string in the 8x8 font. */
int  gfx_text_w(const char *str);

/* Copy a w*h region of `src` (from sx,sy) into `dst` at (dx,dy), clipped. */
void gfx_blit(gfx_surface *dst, int dx, int dy,
              const gfx_surface *src, int sx, int sy, int w, int h);

/* Nearest-neighbour scale the whole of `src` into the dst rect (dx,dy,dw,dh).
 * Used to fit a fixed-size app surface (e.g. DOOM's 640x400) into a window. */
void gfx_blit_scaled(gfx_surface *dst, int dx, int dy, int dw, int dh,
                     const gfx_surface *src);

#endif /* GUI_GFX_H */
