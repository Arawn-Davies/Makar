/* gui_gfx.c -- see gui_gfx.h.  Freestanding; no libc. */
#include "gui_gfx.h"
#include "font8x8.h"            /* FONT8x8[128][8], bit0 = leftmost column */

void gfx_px(gfx_surface *s, int x, int y, gfx_u32 c)
{
    if (!s || !s->px) return;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px[(unsigned)y * (unsigned)s->w + (unsigned)x] = c;
}

void gfx_fill(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c)
{
    if (!s || !s->px) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > s->w) x1 = s->w;
    int y1 = y + h; if (y1 > s->h) y1 = s->h;
    for (int yy = y0; yy < y1; yy++) {
        gfx_u32 *row = &s->px[(unsigned)yy * (unsigned)s->w];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void gfx_outline(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c)
{
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c);
    gfx_fill(s, x + w - 1, y, 1, h, c);
}

void gfx_round(gfx_surface *s, int x, int y, int w, int h, gfx_u32 c, gfx_u32 bg)
{
    gfx_fill(s, x, y, w, h, c);
    gfx_px(s, x, y, bg);             gfx_px(s, x + w - 1, y, bg);
    gfx_px(s, x, y + h - 1, bg);     gfx_px(s, x + w - 1, y + h - 1, bg);
}

void gfx_char(gfx_surface *s, int x, int y, unsigned char ch, gfx_u32 fg)
{
    if (ch >= 128) ch = '?';
    const gfx_u8 *g = FONT8x8[ch];
    for (int row = 0; row < 8; row++) {
        gfx_u8 bits = g[row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col)) gfx_px(s, x + col, y + row, fg);
    }
}

void gfx_str(gfx_surface *s, int x, int y, const char *str, gfx_u32 fg)
{
    for (; *str; str++, x += 8) gfx_char(s, x, y, (unsigned char)*str, fg);
}

void gfx_str_clip(gfx_surface *s, int x, int y, const char *str, gfx_u32 fg, int x_max)
{
    for (; *str && x + 8 <= x_max; str++, x += 8)
        gfx_char(s, x, y, (unsigned char)*str, fg);
}

int gfx_text_w(const char *str)
{
    int n = 0; while (str[n]) n++; return n * 8;
}

void gfx_char_scaled(gfx_surface *s, int x, int y, unsigned char ch, gfx_u32 fg, int scale)
{
    if (scale <= 1) { gfx_char(s, x, y, ch, fg); return; }
    if (ch >= 128) ch = '?';
    const gfx_u8 *g = FONT8x8[ch];
    for (int row = 0; row < 8; row++) {
        gfx_u8 bits = g[row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col))
                gfx_fill(s, x + col * scale, y + row * scale, scale, scale, fg);
    }
}

void gfx_str_scaled(gfx_surface *s, int x, int y, const char *str, gfx_u32 fg, int scale)
{
    int adv = 8 * (scale > 1 ? scale : 1);
    for (; *str; str++, x += adv) gfx_char_scaled(s, x, y, (unsigned char)*str, fg, scale);
}

void gfx_blit(gfx_surface *dst, int dx, int dy,
              const gfx_surface *src, int sx, int sy, int w, int h)
{
    if (!dst || !dst->px || !src || !src->px) return;
    for (int yy = 0; yy < h; yy++) {
        int ddy = dy + yy, ssy = sy + yy;
        if (ddy < 0 || ddy >= dst->h || ssy < 0 || ssy >= src->h) continue;
        for (int xx = 0; xx < w; xx++) {
            int ddx = dx + xx, ssx = sx + xx;
            if (ddx < 0 || ddx >= dst->w || ssx < 0 || ssx >= src->w) continue;
            dst->px[(unsigned)ddy * dst->w + ddx] =
                src->px[(unsigned)ssy * src->w + ssx];
        }
    }
}

void gfx_blit_scaled(gfx_surface *dst, int dx, int dy, int dw, int dh,
                     const gfx_surface *src)
{
    if (!dst || !dst->px || !src || !src->px || dw <= 0 || dh <= 0) return;
    if (src->w <= 0 || src->h <= 0) return;
    for (int yy = 0; yy < dh; yy++) {
        int ddy = dy + yy;
        if (ddy < 0 || ddy >= dst->h) continue;
        int ssy = yy * src->h / dh;
        const gfx_u32 *srow = &src->px[(unsigned)ssy * src->w];
        gfx_u32 *drow = &dst->px[(unsigned)ddy * dst->w];
        for (int xx = 0; xx < dw; xx++) {
            int ddx = dx + xx;
            if (ddx < 0 || ddx >= dst->w) continue;
            drow[ddx] = srow[xx * src->w / dw];
        }
    }
}
