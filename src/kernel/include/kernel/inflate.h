#ifndef _KERNEL_INFLATE_H
#define _KERNEL_INFLATE_H

#include <stdint.h>

/*
 * Decompress a raw DEFLATE stream (RFC 1951 -- no zlib/gzip wrapper, which is
 * exactly what ZIP stores for method 8).  Writes up to dst_cap bytes into dst.
 * Returns 0 on success with *out_len set to the number of bytes produced, or a
 * negative value on a malformed stream or output overflow.
 */
int inflate_raw(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *out_len);

#endif /* _KERNEL_INFLATE_H */
