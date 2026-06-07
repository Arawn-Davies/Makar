/*
 * img_bmp.h -- shared uncompressed-BMP decoder for the Makar GUI.
 *
 * Used by the image viewer (mximg) and the window manager (desktop icons).
 * Handles 24/32-bpp uncompressed BMP only (the format tools/mkicons.py and
 * tests/bmp2png.py emit); BGR(A) on disk is converted to XRGB8888.
 */
#ifndef IMG_BMP_H
#define IMG_BMP_H

#include "gui_gfx.h"

/* Decode an uncompressed 24/32-bpp BMP held in file[0..n) into `out`
 * (row-major XRGB8888, top-down), bounded by out_max_w/out_max_h.  On success
 * returns 0 and writes the image size to *w,*h; returns -1 on any error (bad
 * magic, unsupported bpp/compression, too large, truncated). */
int bmp_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h);

/* Open `path`, read and decode a BMP, and mmap a w*h pixel buffer into
 * out->px (out->w/h set).  Returns 0 on success, -1 on any failure (caller's
 * out is left untouched on failure).  Caller may sys_munmap(out->px, ...) when
 * done; for a handful of small icons it's fine to leave mapped for the run. */
int bmp_load(const char *path, gfx_surface *out);

#endif /* IMG_BMP_H */
