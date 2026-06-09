/*
 * img_ico.h -- shared Windows ICO decoder for the Makar GUI.
 *
 * An .ico is a small directory of images at different sizes; each entry is
 * either a PNG (Vista+ icons -- decoded via the shared img_png) or a bottom-up
 * BMP "DIB" (BITMAPINFOHEADER + optional palette + XOR colour bitmap + 1bpp AND
 * mask).  We pick the largest entry that fits the caller's bound and decode it
 * to opaque XRGB8888; the AND mask / alpha is discarded (the GUI composites
 * icon tiles opaquely, like the BMP path -- see img_bmp.h).  Supported DIB
 * depths: 32/24-bpp and 8/4/1-bit palettised, BI_RGB (uncompressed) only.
 */
#ifndef IMG_ICO_H
#define IMG_ICO_H

#include "gui_gfx.h"

/* Decode the best-fitting image in an ICO held in file[0..n) into `out`
 * (row-major XRGB8888, top-down, pitch == decoded width), bounded by
 * out_max_w/out_max_h.  On success returns 0 and writes the chosen size to
 * *w,*h; on failure returns -1 and, if `err` is non-NULL, copies a short reason
 * into err[0..errcap). */
int ico_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap);

/* Open `path`, read and decode an ICO, and mmap a w*h pixel buffer into
 * out->px (out->w/h set).  Returns 0 on success, -1 on any failure (out left
 * untouched on failure).  Mirrors bmp_load (img_bmp.h). */
int ico_load(const char *path, gfx_surface *out);

#endif /* IMG_ICO_H */
