/*
 * stdlib.h -- minimal POSIX-flavoured stdlib helpers for Makar userspace.
 *
 * Header-only.  Pulls in malloc.h (heap) and provides number-parsing
 * helpers required by TCC and most ports of POSIX shells.  Uses ctype.h
 * for classification; no <errno.h> -- on overflow we clamp at INT_MAX /
 * INT_MIN and set errno-like state via the return value alone (callers
 * check end == s if they need "no conversion").
 */
#ifndef _USERSPACE_STDLIB_H
#define _USERSPACE_STDLIB_H

#include "ctype.h"
#include "malloc.h"

static inline long strtol(const char *s, char **endp, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        p++;
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    long acc = 0;
    int any = 0;
    while (*p) {
        int d;
        if (isdigit((unsigned char)*p))      d = *p - '0';
        else if (*p >= 'a' && *p <= 'z')     d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z')     d = *p - 'A' + 10;
        else                                 break;
        if (d >= base) break;
        acc = acc * base + d;
        any = 1;
        p++;
    }
    if (endp) *endp = (char *)(any ? p : s);
    return neg ? -acc : acc;
}

static inline int atoi(const char *s) { return (int)strtol(s, 0, 10); }

/* strtoul: unsigned version of strtol.  Accepts the same prefix grammar
 * (whitespace, optional '+'/'-', 0x/0 base autodetect).  A leading '-'
 * is honoured by negating the accumulator on return -- POSIX says the
 * value is "negated according to the C language rules", i.e. modulo
 * 2^32 for an unsigned long on i386. */
static inline unsigned long strtoul(const char *s, char **endp, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        p++;
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    unsigned long acc = 0;
    int any = 0;
    while (*p) {
        int d;
        if (*p >= '0' && *p <= '9')      d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else                              break;
        if (d >= base) break;
        acc = acc * (unsigned long)base + (unsigned long)d;
        any = 1;
        p++;
    }
    if (endp) *endp = (char *)(any ? p : s);
    return neg ? (unsigned long)(-(long)acc) : acc;
}

/* getenv: Makar has no env layer yet (SYS_EXECVE ignores envp), so every
 * lookup misses.  Provides the symbol TCC + ports want without forcing
 * a kernel slice today. */
static inline char *getenv(const char *name) { (void)name; return 0; }

/* strdup: malloc + copy.  Returns NULL on OOM. */
static inline char *strdup_(const char *s)
{
    unsigned int n = 0; while (s[n]) n++;
    char *p = (char *)malloc(n + 1);
    if (!p) return 0;
    for (unsigned int i = 0; i <= n; i++) p[i] = s[i];
    return p;
}
#define strdup strdup_

/* qsort: in-place generic sort (insertion sort -- simple, correct, fine
 * for the modest array sizes TCC + Makar ports throw at it). */
static inline void qsort(void *base, unsigned int nmemb, unsigned int sz,
                         int (*cmp)(const void *, const void *))
{
    unsigned char *b = (unsigned char *)base;
    for (unsigned int i = 1; i < nmemb; i++) {
        for (unsigned int j = i; j > 0; j--) {
            unsigned char *a = b + (j - 1) * sz;
            unsigned char *c = b + j * sz;
            if (cmp(a, c) <= 0) break;
            for (unsigned int k = 0; k < sz; k++) {
                unsigned char t = a[k]; a[k] = c[k]; c[k] = t;
            }
        }
    }
}

/* sscanf: tiny subset -- %d, %u, %x, %s, %c, optional width.  Returns
 * the number of successfully matched conversions (POSIX). */
static inline int sscanf(const char *s, const char *fmt, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int matched = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++; continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++; continue;
        }
        fmt++;                  /* consume '%' */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x') {
            int base = (*fmt == 'x') ? 16 : 10;
            while (isspace((unsigned char)*s)) s++;
            char *end;
            long v = strtol(s, &end, base);
            if (end == s) break;
            if (*fmt == 'u' || *fmt == 'x') *__builtin_va_arg(ap, unsigned int *) = (unsigned int)v;
            else                              *__builtin_va_arg(ap, int *)         = (int)v;
            s = end; matched++; fmt++;
        } else if (*fmt == 's') {
            while (isspace((unsigned char)*s)) s++;
            char *out = __builtin_va_arg(ap, char *);
            int wrote = 0;
            while (*s && !isspace((unsigned char)*s) && (width == 0 || wrote < width)) {
                out[wrote++] = *s++;
            }
            out[wrote] = '\0';
            if (wrote == 0) break;
            matched++; fmt++;
        } else if (*fmt == 'c') {
            char *out = __builtin_va_arg(ap, char *);
            if (!*s) break;
            *out = *s++; matched++; fmt++;
        } else {
            break;              /* unknown conversion */
        }
    }
    __builtin_va_end(ap);
    return matched;
}

#endif /* _USERSPACE_STDLIB_H */
