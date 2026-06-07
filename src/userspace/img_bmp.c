/*
 * img_bmp.c -- shared uncompressed-BMP decoder (see img_bmp.h).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "img_bmp.h"

#define RGB GFX_RGB

static unsigned rd32(const unsigned char *p)
{ return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static int rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

int bmp_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h)
{
    if (n < 54 || file[0] != 'B' || file[1] != 'M') return -1;
    unsigned off = rd32(file + 10);
    int iw = (int)rd32(file + 18);
    int ih = (int)rd32(file + 22);
    int bpp = rd16(file + 28);
    unsigned comp = rd32(file + 30);
    int topdown = 0;
    if (ih < 0) { ih = -ih; topdown = 1; }
    if (comp != 0 || (bpp != 24 && bpp != 32)) return -1;
    if (iw < 1 || ih < 1 || iw > out_max_w || ih > out_max_h) return -1;

    int bypp = bpp / 8;
    unsigned stride = ((unsigned)(iw * bypp) + 3u) & ~3u;
    if (off + stride * (unsigned)ih > n) return -1;

    for (int y = 0; y < ih; y++) {
        int srcrow = topdown ? y : (ih - 1 - y);
        const unsigned char *row = file + off + (unsigned)srcrow * stride;
        gfx_u32 *dst = out + (unsigned)y * iw;
        for (int x = 0; x < iw; x++) {
            const unsigned char *px = row + x * bypp;
            dst[x] = RGB(px[2], px[1], px[0]);   /* BMP is BGR(A) */
        }
    }
    *w = iw; *h = ih;
    return 0;
}

/* Bound for bmp_load's one-shot decode (covers desktop icons comfortably). */
#define BMP_LOAD_MAXW 256
#define BMP_LOAD_MAXH 256

int bmp_load(const char *path, gfx_surface *out)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long sz = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, 0);
    if (sz <= 54 || sz > (long)(BMP_LOAD_MAXW * BMP_LOAD_MAXH * 4 + 1024)) {
        sys_close(fd); return -1;
    }

    unsigned long cap = (unsigned long)sz;
    unsigned char *fbuf = (unsigned char *)sys_mmap(0, cap,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fbuf == (unsigned char *)MAP_FAILED) { sys_close(fd); return -1; }

    unsigned got = 0;
    while (got < (unsigned)sz) {
        long r = sys_read(fd, fbuf + got, (unsigned)sz - got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    sys_close(fd);

    /* Peek the dimensions so we can size the pixel buffer exactly. */
    if (got < 54 || fbuf[0] != 'B' || fbuf[1] != 'M') {
        sys_munmap(fbuf, cap); return -1;
    }
    int iw = (int)rd32(fbuf + 18);
    int ih = (int)rd32(fbuf + 22);
    if (ih < 0) ih = -ih;
    if (iw < 1 || ih < 1 || iw > BMP_LOAD_MAXW || ih > BMP_LOAD_MAXH) {
        sys_munmap(fbuf, cap); return -1;
    }

    unsigned long pxbytes = (unsigned long)iw * (unsigned long)ih * 4u;
    gfx_u32 *px = (gfx_u32 *)sys_mmap(0, pxbytes,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (px == (gfx_u32 *)MAP_FAILED) { sys_munmap(fbuf, cap); return -1; }

    int dw, dh;
    if (bmp_decode(fbuf, got, px, iw, ih, &dw, &dh) != 0) {
        sys_munmap(px, pxbytes); sys_munmap(fbuf, cap); return -1;
    }
    sys_munmap(fbuf, cap);

    out->px = px; out->w = dw; out->h = dh;
    return 0;
}
