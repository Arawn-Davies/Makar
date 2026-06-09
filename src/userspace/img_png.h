/*
 * img_png.h -- shared PNG decoder for the Makar GUI.
 *
 * Self-contained: a from-scratch RFC1951 inflater (stored / fixed / dynamic
 * Huffman) feeds an RFC2083 PNG reader (all five scanline filters, bit depths
 * 1/2/4/8/16, colour types 0/2/3/4/6).  No zlib, no interlace (Adam7 images are
 * rejected with a clear error).  Alpha is discarded -- the GUI has no alpha
 * compositing, matching the BMP path (see img_bmp.h).
 */
#ifndef IMG_PNG_H
#define IMG_PNG_H

#include "gui_gfx.h"

/* Decode a PNG held in file[0..n) into `out` (row-major XRGB8888, top-down),
 * bounded by out_max_w/out_max_h.  On success returns 0 and writes the image
 * size to *w,*h.  On failure returns -1 and, if `err` is non-NULL, copies a
 * short human-readable reason into err[0..errcap) (e.g. "interlaced PNG
 * unsupported", "truncated PNG").  Uses sys_mmap for transient scratch and
 * frees it before returning. */
int png_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap);

/* Open `path`, read and decode a PNG, and mmap a w*h pixel buffer into out->px
 * (out->w/h set).  Returns 0 on success, -1 on any failure (out left untouched
 * on failure).  Mirrors bmp_load (img_bmp.h); bounded to 1024x1024. */
int png_load(const char *path, gfx_surface *out);

#endif /* IMG_PNG_H */
