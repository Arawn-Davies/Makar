#ifndef _KERNEL_WGET_H
#define _KERNEL_WGET_H

#include <stdint.h>

/*
 * Fetch an "http://host[:port]/path" URL over lwIP TCP (no TLS).
 *
 * On success returns 0 and sets:
 *   *out_body   – kmalloc'd buffer holding the (de-chunked) response body;
 *                 the caller must kfree() it.
 *   *out_len    – body length in bytes.
 *   *out_status – HTTP status code (e.g. 200).
 * Any of the out pointers may be NULL.  Returns a negative value on error
 * (bad URL, no network, DNS/connect failure, malformed response).
 */
int wget_fetch(const char *url, uint8_t **out_body, uint32_t *out_len,
               int *out_status);

#endif /* _KERNEL_WGET_H */
