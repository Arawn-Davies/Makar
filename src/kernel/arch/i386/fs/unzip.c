/*
 * unzip.c -- extract a ZIP archive from memory onto the VFS.
 *
 * Parses the End-of-Central-Directory record and walks the central directory,
 * which carries authoritative sizes (local headers may defer them to a data
 * descriptor).  Stored (method 0) and deflate (method 8, via inflate_raw)
 * entries are supported.  Unsafe names (absolute, or containing "..") are
 * skipped.
 */
#include <kernel/unzip.h>
#include <kernel/inflate.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <string.h>

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CEN_SIG  0x02014b50u
#define ZIP_LOC_SIG  0x04034b50u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reject path traversal / absolute names. */
static int name_is_unsafe(const char *name, uint16_t nlen)
{
    if (nlen == 0) return 1;
    if (name[0] == '/' || name[0] == '\\') return 1;
    for (uint16_t i = 0; i < nlen; i++) {
        if (name[i] == '\\') return 1;          /* keep names POSIX */
        if (name[i] == '.' && i + 1 < nlen && name[i + 1] == '.' &&
            (i == 0 || name[i - 1] == '/') &&
            (i + 2 == nlen || name[i + 2] == '/'))
            return 1;                            /* ".." path component */
    }
    return 0;
}

/* mkdir each directory prefix of path (mkdir -p of the parent chain). */
static void make_parent_dirs(const char *path)
{
    char buf[VFS_PATH_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; path[i] && n < sizeof(buf) - 1; i++) {
        if (path[i] == '/' && n > 0) {
            buf[n] = '\0';
            vfs_mkdir(buf);                      /* idempotent: ignore errors */
        }
        buf[n++] = path[i];
    }
}

/* dest = destdir "/" name(nlen).  Returns length, or 0 if it would overflow. */
static uint32_t build_dest(char *dest, uint32_t cap, const char *destdir,
                           const char *name, uint16_t nlen)
{
    uint32_t d = 0;
    for (const char *c = destdir; *c && d < cap - 1; c++) dest[d++] = *c;
    if (d > 0 && dest[d - 1] != '/' && d < cap - 1) dest[d++] = '/';
    for (uint16_t k = 0; k < nlen && d < cap - 1; k++) dest[d++] = name[k];
    if (d >= cap - 1) return 0;
    dest[d] = '\0';
    return d;
}

int unzip_archive(const uint8_t *zip, uint32_t len, const char *destdir,
                  int *out_failed)
{
    if (out_failed) *out_failed = 0;
    if (!zip || !destdir || len < 22)
        return -1;

    /* Find the EOCD by scanning backwards from the end (it sits before an
     * optional comment of up to 65535 bytes). */
    uint32_t eocd = 0;
    int found = 0;
    uint32_t limit = (len > 22 + 65535u) ? (len - 22 - 65535u) : 0;
    for (uint32_t i = len - 22; ; i--) {
        if (rd32(zip + i) == ZIP_EOCD_SIG) { eocd = i; found = 1; break; }
        if (i <= limit) break;
    }
    if (!found)
        return -1;

    uint16_t total = rd16(zip + eocd + 10);
    uint32_t cd_off = rd32(zip + eocd + 16);
    if (cd_off >= len)
        return -1;

    int extracted = 0, failed = 0;
    uint32_t p = cd_off;
    for (uint16_t idx = 0; idx < total; idx++) {
        if (p + 46 > len || rd32(zip + p) != ZIP_CEN_SIG)
            break;
        uint16_t method = rd16(zip + p + 10);
        uint32_t csize  = rd32(zip + p + 20);
        uint32_t usize  = rd32(zip + p + 24);
        uint16_t nlen   = rd16(zip + p + 28);
        uint16_t elen   = rd16(zip + p + 30);
        uint16_t clen   = rd16(zip + p + 32);
        uint32_t lh_off = rd32(zip + p + 42);
        const char *name = (const char *)(zip + p + 46);
        if (p + 46 + nlen > len) break;

        uint32_t next = p + 46 + nlen + elen + clen;

        if (name_is_unsafe(name, nlen)) { failed++; p = next; continue; }

        char dest[VFS_PATH_MAX];
        uint32_t dl = build_dest(dest, sizeof(dest), destdir, name, nlen);
        if (dl == 0) { failed++; p = next; continue; }

        if (dest[dl - 1] == '/') {               /* directory entry */
            dest[dl - 1] = '\0';
            make_parent_dirs(dest);
            vfs_mkdir(dest);
            p = next;
            continue;
        }

        make_parent_dirs(dest);

        /* Locate the entry data via its local header. */
        if (lh_off + 30 > len || rd32(zip + lh_off) != ZIP_LOC_SIG) {
            failed++; p = next; continue;
        }
        uint16_t l_nlen = rd16(zip + lh_off + 26);
        uint16_t l_elen = rd16(zip + lh_off + 28);
        uint32_t data_off = lh_off + 30u + l_nlen + l_elen;
        if ((uint64_t)data_off + csize > len) { failed++; p = next; continue; }
        const uint8_t *data = zip + data_off;

        if (method == 0) {                        /* stored */
            if (csize != usize) { failed++; }
            else if (vfs_write_file(dest, data, usize) == 0) extracted++;
            else failed++;
        } else if (method == 8) {                 /* deflate */
            uint8_t *out = (uint8_t *)kmalloc(usize ? usize : 1u);
            uint32_t got = 0;
            if (out && inflate_raw(data, csize, out, usize, &got) == 0 &&
                got == usize && vfs_write_file(dest, out, usize) == 0)
                extracted++;
            else
                failed++;
            kfree(out);
        } else {
            failed++;                             /* unsupported method */
        }

        p = next;
    }

    if (out_failed) *out_failed = failed;
    return extracted;
}
