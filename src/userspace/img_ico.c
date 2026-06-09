/*
 * img_ico.c -- shared Windows ICO decoder (see img_ico.h).
 *
 * Parses the ICONDIR + ICONDIRENTRY table, picks the largest entry that fits
 * the caller's bound, and decodes it: PNG entries go through the shared
 * png_decode (img_png.c); BMP-DIB entries are unfiltered here (bottom-up XOR
 * bitmap, 32/24-bpp or 8/4/1-bit palettised).  The 1bpp AND transparency mask
 * is skipped -- icons composite opaquely, matching the BMP path.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "img_png.h"
#include "img_ico.h"

#define RGB GFX_RGB

static unsigned rd32(const unsigned char *p)
{ return p[0] | (p[1]<<8) | (p[2]<<16) | ((unsigned)p[3]<<24); }
static int rd16(const unsigned char *p) { return p[0] | (p[1]<<8); }

static void ecpy(char *d, const char *s, int cap)
{ if(!d||cap<=0) return; int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }

/* Decode a bottom-up BMP "DIB" (no file header) from d[0..n) into `out`. */
static int dib_decode(const unsigned char *d, unsigned n, gfx_u32 *out,
                      int mw, int mh, int *w, int *h, char *err, int errcap)
{
    if (n < 40) { ecpy(err, "truncated ICO image", errcap); return -1; }
    unsigned hsize = rd32(d);
    if (hsize < 40 || hsize > n) { ecpy(err, "unsupported ICO image", errcap); return -1; }
    int iw = (int)rd32(d + 4);
    int ih = (int)rd32(d + 8);              /* doubled: XOR rows + AND mask rows */
    int bpp = rd16(d + 14);
    unsigned comp = rd32(d + 16);
    if (comp != 0) { ecpy(err, "compressed ICO unsupported", errcap); return -1; }
    int realh = ih / 2; if (realh < 1) realh = ih;
    if (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1) {
        ecpy(err, "unsupported ICO depth", errcap); return -1;
    }
    if (iw < 1 || realh < 1 || iw > mw || realh > mh) {
        ecpy(err, "icon too large", errcap); return -1;
    }

    const unsigned char *pal = d + hsize;
    int palcount = 0;
    if (bpp <= 8) {
        unsigned clrused = rd32(d + 32);
        palcount = clrused ? (int)clrused : (1 << bpp);
    }
    const unsigned char *xorb = pal + palcount * 4;
    unsigned xstride = (((unsigned)(iw * bpp) + 31) / 32) * 4;
    if (xorb + (unsigned long)xstride * realh > d + n) {
        ecpy(err, "truncated ICO image", errcap); return -1;
    }

    for (int y = 0; y < realh; y++) {
        const unsigned char *row = xorb + (unsigned long)(realh - 1 - y) * xstride;
        gfx_u32 *dst = out + (unsigned long)y * iw;
        for (int x = 0; x < iw; x++) {
            int r, g, b;
            if (bpp == 32)       { const unsigned char *p = row + x*4; b=p[0]; g=p[1]; r=p[2]; }
            else if (bpp == 24)  { const unsigned char *p = row + x*3; b=p[0]; g=p[1]; r=p[2]; }
            else {
                int idx;
                if (bpp == 8)      idx = row[x];
                else if (bpp == 4) idx = (row[x>>1] >> ((x&1) ? 0 : 4)) & 0xf;
                else               idx = (row[x>>3] >> (7 - (x&7))) & 1;
                const unsigned char *pe = pal + idx*4; b=pe[0]; g=pe[1]; r=pe[2];
            }
            dst[x] = RGB(r, g, b);
        }
    }
    *w = iw; *h = realh;
    return 0;
}

int ico_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap)
{
    if (n < 6) { ecpy(err, "truncated ICO", errcap); return -1; }
    if (file[0] || file[1] || file[2] != 1 || file[3] != 0) {
        ecpy(err, "not an ICO file", errcap); return -1;
    }
    int count = rd16(file + 4);
    if (count < 1) { ecpy(err, "empty ICO", errcap); return -1; }
    if (6 + (unsigned)count * 16 > n) { ecpy(err, "truncated ICO", errcap); return -1; }

    /* Pick the largest entry that fits the bound; if none fit, the smallest. */
    int best = -1; long bestscore = 0;
    for (int i = 0; i < count; i++) {
        const unsigned char *e = file + 6 + i * 16;
        int ew = e[0] ? e[0] : 256, eh = e[1] ? e[1] : 256;
        long area = (long)ew * eh;
        long score = (ew <= out_max_w && eh <= out_max_h) ? area : -area;
        if (best < 0 || score > bestscore) { best = i; bestscore = score; }
    }
    const unsigned char *e = file + 6 + best * 16;
    unsigned size = rd32(e + 8), off = rd32(e + 12);
    if (size < 1 || off + size > n) { ecpy(err, "corrupt ICO entry", errcap); return -1; }

    const unsigned char *data = file + off;
    if (size >= 8 && data[0]==0x89 && data[1]=='P' && data[2]=='N' && data[3]=='G')
        return png_decode(data, size, out, out_max_w, out_max_h, w, h, err, errcap);
    return dib_decode(data, size, out, out_max_w, out_max_h, w, h, err, errcap);
}

/* Largest icon we'll load (covers any reasonable desktop/menu glyph). */
#define ICO_MAXW 256
#define ICO_MAXH 256

int ico_load(const char *path, gfx_surface *out)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long sz = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, 0);
    if (sz <= 6 || sz > 1024L * 1024L) { sys_close(fd); return -1; }

    unsigned long cap = (unsigned long)sz;
    unsigned char *fbuf = (unsigned char *)sys_mmap(0, cap, PROT_READ|PROT_WRITE,
                                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (fbuf == (unsigned char *)MAP_FAILED) { sys_close(fd); return -1; }
    unsigned got = 0;
    while (got < (unsigned)sz) {
        long r = sys_read(fd, fbuf + got, (unsigned)sz - got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    sys_close(fd);

    unsigned long scratch_bytes = (unsigned long)ICO_MAXW * ICO_MAXH * 4u;
    gfx_u32 *scratch = (gfx_u32 *)sys_mmap(0, scratch_bytes, PROT_READ|PROT_WRITE,
                                           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (scratch == (gfx_u32 *)MAP_FAILED) { sys_munmap(fbuf, cap); return -1; }

    int dw, dh; char err[64];
    if (ico_decode(fbuf, got, scratch, ICO_MAXW, ICO_MAXH, &dw, &dh, err, sizeof err) != 0) {
        sys_munmap(scratch, scratch_bytes); sys_munmap(fbuf, cap); return -1;
    }
    sys_munmap(fbuf, cap);

    /* Copy the packed dw*dh pixels into an exactly-sized surface buffer. */
    unsigned long pxbytes = (unsigned long)dw * dh * 4u;
    gfx_u32 *px = (gfx_u32 *)sys_mmap(0, pxbytes, PROT_READ|PROT_WRITE,
                                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (px == (gfx_u32 *)MAP_FAILED) { sys_munmap(scratch, scratch_bytes); return -1; }
    for (unsigned long i = 0; i < (unsigned long)dw * dh; i++) px[i] = scratch[i];
    sys_munmap(scratch, scratch_bytes);

    out->px = px; out->w = dw; out->h = dh;
    return 0;
}
