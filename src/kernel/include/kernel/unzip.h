#ifndef _KERNEL_UNZIP_H
#define _KERNEL_UNZIP_H

#include <stdint.h>

/*
 * Extract a ZIP archive held in memory into destdir on the VFS.
 *
 * Walks the central directory; supports stored (method 0) and deflate
 * (method 8) entries, creates intermediate directories, and rejects unsafe
 * names (absolute or containing "..").  Returns the number of files
 * successfully written, or -1 on a structurally invalid archive.  If
 * out_failed is non-NULL it receives the count of entries that could not be
 * extracted (unsupported method, bad data, or write failure).
 */
int unzip_archive(const uint8_t *zip, uint32_t len, const char *destdir,
                  int *out_failed);

#endif /* _KERNEL_UNZIP_H */
