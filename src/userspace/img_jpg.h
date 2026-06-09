/*
 * img_jpg.h -- shared baseline-JPEG decoder for the Makar GUI.
 *
 * Self-contained, integer-only (no libc/libm): marker parse + Huffman entropy
 * decode + dequantise + a fixed-point 8x8 IDCT + chroma upsampling + YCbCr->RGB.
 * Handles baseline sequential DCT (SOF0), 8-bit, 1 component (grayscale) or 3
 * (YCbCr) with 4:4:4 / 4:2:2 / 4:2:0 sampling, and restart markers.  Progressive
 * (SOF2) and arithmetic coding are rejected with a clear error.
 */
#ifndef IMG_JPG_H
#define IMG_JPG_H

#include "gui_gfx.h"

/* Decode a baseline JPEG held in file[0..n) into `out` (row-major XRGB8888,
 * top-down, pitch == width), bounded by out_max_w/out_max_h.  On success
 * returns 0 and writes the size to *w,*h; on failure returns -1 and copies a
 * short reason into err[0..errcap) (if err != NULL).  Uses sys_mmap for scratch
 * and frees it before returning. */
int jpg_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap);

#endif /* IMG_JPG_H */
