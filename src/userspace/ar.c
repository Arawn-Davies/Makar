/*
 * ar.c -- minimal BSD-style `ar rcs <archive> <object>...` for Makar.
 *
 * Implements just enough of the System V / BSD ar(1) format to let
 * in-OS TCC stitch a set of .o files into a libk.a / libc.a.  The
 * archive smoke is part of the self-hosting regression ladder; see the
 * remaining compiler self-rebuild work in docs/plans/v1.md.
 *
 * Supported operations:
 *   ar rcs OUT IN1 [IN2 ...]    create/replace OUT; add IN*; "ranlib"
 *                                quietly ignored (we don't write the
 *                                /SYMDEF table -- TCC's linker scans
 *                                members directly).
 *
 * Unsupported: extract / list / delete / extended-name table.  Names
 * longer than 15 chars are truncated -- callers should rename
 * accordingly (e.g. shorten `something_with_a_long_name.o`).
 *
 * Format (per object, after the leading "!<arch>\n" magic):
 *   char name[16];    /-suffixed, space-padded
 *   char date[12];    decimal seconds since epoch, space-padded
 *   char uid[6];      "0     "
 *   char gid[6];      "0     "
 *   char mode[8];     octal "644     "
 *   char size[10];    decimal, space-padded
 *   char magic[2];    "`\n"
 *   <file contents, padded to even-byte boundary with \n>
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

static int put_str(int fd, const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return (int)sys_write(fd, s, n);
}

static void put_err(const char *s) { put_str(2, s); }

/* Right-pad `src` (length len) into `dst[width]` with spaces.  No NUL --
 * ar headers are fixed-width and adjacent fields butt up against each
 * other; the trailing "`\n" magic is what readers anchor on. */
static void pad_field(char *dst, int width, const char *src, int len)
{
    int i = 0;
    while (i < len && i < width) { dst[i] = src[i]; i++; }
    while (i < width) dst[i++] = ' ';
}

static void fmt_dec(char *buf, int width, unsigned long v)
{
    char tmp[24];
    int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + (v % 10u)); v /= 10u; }
    int j = 0;
    while (ti > 0) buf[j++] = tmp[--ti];
    while (j < width) buf[j++] = ' ';
}

/* Strip directory components from `path` so the archive member name is
 * just the basename. */
static const char *basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    return base;
}

static int copy_into_archive(int outfd, const char *path)
{
    int infd = sys_open(path, O_RDONLY);
    if (infd < 0) {
        put_err("ar: cannot open ");
        put_err(path);
        put_err("\n");
        return -1;
    }
    long size = sys_lseek(infd, 0, SEEK_END);
    if (size < 0) { sys_close(infd); return -1; }
    sys_lseek(infd, 0, SEEK_SET);

    /* Header: 60 bytes. */
    char hdr[60];
    const char *name = basename(path);
    /* ar's classic name field: name + `/`, space-padded to 16. */
    int nl = 0; while (name[nl]) nl++;
    char namebuf[17];
    int ni = 0;
    while (ni < nl && ni < 15) { namebuf[ni] = name[ni]; ni++; }
    namebuf[ni++] = '/';
    namebuf[ni] = '\0';
    pad_field(hdr + 0, 16, namebuf, ni);

    fmt_dec(hdr + 16, 12, 0);              /* date: epoch 0 (deterministic) */
    pad_field(hdr + 28, 6, "0", 1);        /* uid */
    pad_field(hdr + 34, 6, "0", 1);        /* gid */
    pad_field(hdr + 40, 8, "644", 3);      /* mode (octal) */
    fmt_dec(hdr + 48, 10, (unsigned long)size);
    hdr[58] = '`';
    hdr[59] = '\n';

    sys_write(outfd, hdr, 60);

    /* Copy contents in 4 KiB chunks. */
    char buf[4096];
    long remaining = size;
    while (remaining > 0) {
        unsigned int want = (remaining > (long)sizeof(buf))
                          ? sizeof(buf) : (unsigned int)remaining;
        long got = sys_read(infd, buf, want);
        if (got <= 0) break;
        sys_write(outfd, buf, (unsigned int)got);
        remaining -= got;
    }
    sys_close(infd);

    /* Pad to even byte if file size is odd. */
    if (size & 1) {
        char nl = '\n';
        sys_write(outfd, &nl, 1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        put_err("Usage: ar rcs ARCHIVE OBJECT [OBJECT...]\n");
        return 1;
    }
    /* Accept any subset of {r, c, s, v} in the op string; we always
     * create + replace + (no-op) symbol-table. */
    const char *op = argv[1];
    int valid = 1;
    for (int i = 0; op[i]; i++) {
        if (op[i] != 'r' && op[i] != 'c' && op[i] != 's' && op[i] != 'v') {
            valid = 0; break;
        }
    }
    if (!valid) {
        put_err("ar: only `rcs` (+v) operations supported\n");
        return 1;
    }

    const char *archive = argv[2];
    int outfd = sys_open(archive, O_WRONLY | O_CREAT | O_TRUNC);
    if (outfd < 0) {
        put_err("ar: cannot create ");
        put_err(archive);
        put_err("\n");
        return 1;
    }
    sys_write(outfd, "!<arch>\n", 8);

    int errs = 0;
    for (int i = 3; i < argc; i++) {
        if (copy_into_archive(outfd, argv[i]) != 0) errs++;
    }
    sys_close(outfd);
    return errs ? 2 : 0;
}
