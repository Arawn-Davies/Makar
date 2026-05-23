/*
 * string.c -- userspace string / memory helpers.  See string.h.
 *
 * Byte-at-a-time implementations.  Word-at-a-time is a tempting next
 * step, but TCC (the headline consumer of this shim) is bounded by
 * tokenisation latency, not memcpy throughput; the simpler code is
 * easier to audit and saves <50 LoC of word-alignment dance.
 */
#include "string.h"

void *memcpy(void *dst, const void *src, string_size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (string_size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, string_size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (string_size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (string_size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *dst, int c, string_size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    unsigned char        b = (unsigned char)c;
    for (string_size_t i = 0; i < n; i++) d[i] = b;
    return dst;
}

int memcmp(const void *a, const void *b, string_size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (string_size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}

void *memchr(const void *s, int c, string_size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char        b = (unsigned char)c;
    for (string_size_t i = 0; i < n; i++) {
        if (p[i] == b) return (void *)(p + i);
    }
    return 0;
}

string_size_t strlen(const char *s)
{
    string_size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, string_size_t n)
{
    for (string_size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb || ca == 0) return (int)ca - (int)cb;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, string_size_t n)
{
    string_size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strchr(const char *s, int c)
{
    char target = (char)c;
    for (; *s; s++) {
        if (*s == target) return (char *)s;
    }
    return (target == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    char        target = (char)c;
    const char *last   = 0;
    for (; *s; s++) {
        if (*s == target) last = s;
    }
    if (target == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}
