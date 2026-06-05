/*
 * stdio.c -- userspace FILE-pointer + snprintf implementation.
 *
 * FILE* over fd syscalls: 1 KiB write buffer per stream, line-buffered for
 * stdout (fd 1) and stderr (fd 2), fully-buffered for opened files.
 * No read buffering today (TCC tokenises 1 byte at a time via fgetc but
 * the fd-side eager-load makes that O(1) already).
 */
#include "stdio.h"
#include "malloc.h"
#include "syscall.h"

/* ---- Standard streams ----------------------------------------------------- */
static FILE s_stdin  = { .fd = 0 };
static FILE s_stdout = { .fd = 1 };
static FILE s_stderr = { .fd = 2 };
FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

/* ---- fopen flag parsing --------------------------------------------------- */
static int parse_mode(const char *m)
{
    int wr = 0, app = 0, plus = 0;
    for (const char *p = m; *p; p++) {
        if      (*p == 'r') {}
        else if (*p == 'w') wr  = 1;
        else if (*p == 'a') app = 1;
        else if (*p == '+') plus = 1;
    }
    if (app)  return O_WRONLY | O_CREAT | O_APPEND;
    if (wr)   return O_WRONLY | O_CREAT | O_TRUNC;
    if (plus) return O_RDWR;
    return O_RDONLY;
}

/* ---- Low-level flush of buffered writes ---------------------------------- */
static int flush_writes(FILE *f)
{
    if (f->wlen == 0) return 0;
    long w = sys_write(f->fd, f->wbuf, f->wlen);
    if (w < 0) { f->err = 1; return -1; }
    f->wlen = 0;
    return 0;
}

int fflush(FILE *f) { return f ? flush_writes(f) : 0; }

FILE *fopen(const char *path, const char *mode)
{
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return 0;
    f->fd = sys_open(path, parse_mode(mode));
    if (f->fd < 0) { free(f); return 0; }
    f->err = 0; f->eof = 0; f->wlen = 0;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return -1;
    int rc = flush_writes(f);
    if (sys_close(f->fd) != 0) rc = -1;
    free(f);
    return rc;
}

unsigned int fwrite(const void *p, unsigned int sz, unsigned int n, FILE *f)
{
    if (!f || !p || sz == 0 || n == 0) return 0;
    unsigned int total = sz * n;
    const unsigned char *src = (const unsigned char *)p;
    int line_buffered = (f->fd == 1 || f->fd == 2);
    unsigned int copied = 0;
    while (copied < total) {
        unsigned int room = STDIO_BUFSZ - f->wlen;
        if (room == 0) { if (flush_writes(f) < 0) break; room = STDIO_BUFSZ; }
        unsigned int chunk = (total - copied < room) ? total - copied : room;
        for (unsigned int i = 0; i < chunk; i++) {
            f->wbuf[f->wlen++] = src[copied + i];
            if (line_buffered && src[copied + i] == '\n') {
                if (flush_writes(f) < 0) return copied + i + 1;
            }
        }
        copied += chunk;
    }
    return copied / sz;
}

unsigned int fread(void *p, unsigned int sz, unsigned int n, FILE *f)
{
    if (!f || !p || sz == 0 || n == 0) return 0;
    unsigned int total = sz * n;
    long got = sys_read(f->fd, p, total);
    if (got < 0) { f->err = 1; return 0; }
    if ((unsigned long)got < total) f->eof = 1;
    return (unsigned int)got / sz;
}

int fputs(const char *s, FILE *f)
{
    unsigned int n = 0; while (s[n]) n++;
    return (int)fwrite(s, 1, n, f);
}
int fputc(int c, FILE *f) { unsigned char b = (unsigned char)c; return fwrite(&b,1,1,f)==1 ? c : -1; }
int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { if (fputs(s, stdout) < 0) return -1; return fputc('\n', stdout); }
int fgetc(FILE *f) { unsigned char b; long r = sys_read(f->fd,&b,1); if (r<=0){f->eof=(r==0);return -1;} return b; }
int feof(FILE *f)  { return f ? f->eof : 1; }
/* fseek/ftell are provided by tcc_compat.o (over sys_lseek) -- not redefined. */

/* ---- vsnprintf / snprintf / fprintf / printf ----------------------------- */

typedef struct {
    char *buf; unsigned int cap; unsigned int len;
} sb_t;

static void sb_put(sb_t *s, char c)
{
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void sb_str(sb_t *s, const char *p) { while (*p) sb_put(s, *p++); }

static void sb_num(sb_t *s, unsigned long v, unsigned int base,
                   int upper, int sign_neg, int width, char pad)
{
    char tmp[32]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) {
        unsigned int d = v % base;
        tmp[ti++] = (char)(d < 10 ? '0' + d
                                  : (upper ? 'A' : 'a') + (d - 10));
        v /= base;
    }
    int len = ti + (sign_neg ? 1 : 0);
    if (sign_neg && pad == '0') sb_put(s, '-');
    while (len < width) { sb_put(s, pad); len++; }
    if (sign_neg && pad == ' ') sb_put(s, '-');
    while (ti--) sb_put(s, tmp[ti]);
}

int vsnprintf(char *buf, unsigned int sz, const char *fmt, va_list ap)
{
    sb_t s = { buf, sz, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sb_put(&s, *p); continue; }
        p++;
        /* Flags (minimal subset: `-`, `+`, ` ` ignored; `0` triggers
         * zero-padding; `#` ignored). */
        char pad = ' '; int width = 0;
        if (*p == '-' || *p == '+' || *p == ' ' || *p == '#') p++;
        if (*p == '0') { pad = '0'; p++; }
        while (*p >= '0' && *p <= '9') { width = width*10 + (*p - '0'); p++; }
        /* Optional precision (.N) -- parsed but ignored for now; just
         * keep the va_arg sequence aligned. */
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') p++;
        }
        /* Length modifier: `h`, `hh`, `l`, `ll`, `z`, `t`, `j`.  On i386
         * `long`, `size_t`, `ptrdiff_t`, `intmax_t` are all 32-bit, so
         * a single `l`/`z`/`t`/`j` is a no-op for va_arg consumption.
         * `ll` (long long) IS 64-bit -- mark it so the spec handlers
         * below consume two slots, and format the full value. */
        int is_ll = 0;
        if (*p == 'h') { p++; if (*p == 'h') p++; }
        else if (*p == 'l') { p++; if (*p == 'l') { is_ll = 1; p++; } }
        else if (*p == 'z' || *p == 't' || *p == 'j') { p++; }
        switch (*p) {
        case 'c': sb_put(&s, (char)va_arg(ap, int)); break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            /* Sanity: reject obviously-bogus pointers (outside the
             * user address window USER_CODE_BASE..USER_STACK_TOP)
             * so a stale caller arg doesn't take down the whole
             * task with a SIGSEGV while formatting an error message.
             * Defensive only -- a valid pointer that happens to land
             * outside this window (e.g. an .rodata in a different
             * mapping) prints "(badptr)" instead of crashing. */
            unsigned int up = (unsigned int)(unsigned long)str;
            /* Valid ranges: [USER_CODE_BASE, brk) for code+heap, and
             * the top 32 KiB of the user address space for stack. */
            unsigned int brk_top = (unsigned int)(unsigned long)sys_brk((void *)0);
            int in_codeheap = (up >= 0x40000000u && up < brk_top);
            int in_stack    = (up >= 0xBFFE0000u && up <  0xC0000000u);
            if (!in_codeheap && !in_stack) str = "(badptr)";
            int len = 0; while (str[len]) len++;
            while (len < width) { sb_put(&s, pad); len++; }
            sb_str(&s, str); break;
        }
        case 'd': case 'i': {
            if (is_ll) {
                long long v = va_arg(ap, long long);
                unsigned long long uv = (v < 0)
                    ? (unsigned long long)(-(long long)v)
                    : (unsigned long long)v;
                /* Render via the existing sb_num path -- cast to ulong
                 * is lossy for the full 64-bit range, but a typical
                 * TCC %lld value (line numbers, sizes) fits in 32 bits.
                 * Wrap-around above 2^32 is documented as best-effort. */
                sb_num(&s, (unsigned long)uv, 10, 0, v < 0, width, pad);
            } else {
                int v = va_arg(ap, int);
                unsigned long uv = (v < 0) ? (unsigned long)(-(long)v) : (unsigned long)v;
                sb_num(&s, uv, 10, 0, v < 0, width, pad);
            }
            break;
        }
        case 'u': {
            if (is_ll) {
                unsigned long long v = va_arg(ap, unsigned long long);
                sb_num(&s, (unsigned long)v, 10, 0, 0, width, pad);
            } else {
                sb_num(&s, va_arg(ap, unsigned int), 10, 0, 0, width, pad);
            }
            break;
        }
        case 'x': {
            if (is_ll) {
                unsigned long long v = va_arg(ap, unsigned long long);
                sb_num(&s, (unsigned long)v, 16, 0, 0, width, pad);
            } else {
                sb_num(&s, va_arg(ap, unsigned int), 16, 0, 0, width, pad);
            }
            break;
        }
        case 'X': {
            if (is_ll) {
                unsigned long long v = va_arg(ap, unsigned long long);
                sb_num(&s, (unsigned long)v, 16, 1, 0, width, pad);
            } else {
                sb_num(&s, va_arg(ap, unsigned int), 16, 1, 0, width, pad);
            }
            break;
        }
        case 'o': {
            if (is_ll) {
                unsigned long long v = va_arg(ap, unsigned long long);
                sb_num(&s, (unsigned long)v, 8, 0, 0, width, pad);
            } else {
                sb_num(&s, va_arg(ap, unsigned int), 8, 0, 0, width, pad);
            }
            break;
        }
        case 'p': sb_put(&s,'0'); sb_put(&s,'x');
                  sb_num(&s, (unsigned long)(unsigned int)va_arg(ap, void *), 16, 0, 0, 8, '0'); break;
        case '%': sb_put(&s, '%'); break;
        default:  sb_put(&s, '%'); sb_put(&s, *p); break;
        }
    }
    if (s.cap > 0) s.buf[s.len < s.cap ? s.len : s.cap - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *buf, unsigned int sz, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    static char scratch[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    fwrite(scratch, 1, (unsigned int)n, f);
    return n;
}

int printf(const char *fmt, ...)
{
    static char scratch[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(scratch, sizeof(scratch), fmt, ap);
    va_end(ap);
    fwrite(scratch, 1, (unsigned int)n, stdout);
    return n;
}
