/*
 * tls.c -- TLS 1.2 client over a TCP socket fd, via BearSSL (see tls.h).
 *
 * Ported from the in-kernel scaffold that proved the BearSSL setup but
 * overflowed the 8 KB ring-0 stack; here it runs in ring 3 (32 KB stack, heap-
 * allocated contexts) where it belongs.  The socket I/O callbacks just call
 * read()/write() on the fd -- the kernel socket blocks for us, so there's no
 * manual poll loop.
 */

#include "tls.h"
#include "syscall.h"
#include "bearssl.h"
#include <stdlib.h>     /* malloc / free   (libc.a) */
#include <string.h>     /* memcpy / memset (libc.a) */

/* ---- x509 "no anchor" wrapper: accept any chain (encrypt, don't authenticate).
 * Wraps br_x509_minimal and turns NOT_TRUSTED into success. -------------- */
typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} noanchor_t;

static void na_start_chain(const br_x509_class **ctx, const char *sn)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; (*x->inner)->start_chain(x->inner, sn); }
static void na_start_cert(const br_x509_class **ctx, uint32_t len)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; (*x->inner)->start_cert(x->inner, len); }
static void na_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; (*x->inner)->append(x->inner, buf, len); }
static void na_end_cert(const br_x509_class **ctx)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; (*x->inner)->end_cert(x->inner); }
static unsigned na_end_chain(const br_x509_class **ctx)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; unsigned r = (*x->inner)->end_chain(x->inner);
  return (r == BR_ERR_X509_NOT_TRUSTED) ? 0 : r; }
static const br_x509_pkey *na_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{ noanchor_t *x = (noanchor_t *)(void *)ctx; return (*x->inner)->get_pkey(x->inner, usages); }

static const br_x509_class noanchor_vtable = {
    sizeof(noanchor_t),
    na_start_chain, na_start_cert, na_append, na_end_cert, na_end_chain, na_get_pkey
};

/* ---- DRBG seed.  RDRAND when CPUID advertises it (qemu32 has none -> #UD;
 * in ring 3 that's just a SIGILL to this tab, but CPUID-gate it anyway), mixed
 * with the uptime timer; else a timer-seeded LCG.  Weak -- enough for a hobby
 * handshake, NOT for anything sensitive. ------------------------------------ */
static int cpu_has_rdrand(void)
{
    uint32_t eax, ecx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=c"(ecx) : "a"(1) : "ebx", "edx");
    (void)eax;
    return (ecx >> 30) & 1u;
}

static void seed_bytes(unsigned char *seed, int n)
{
    int has = cpu_has_rdrand();
    uint32_t mix = sys_uptime() ^ (uint32_t)sys_getpid();
    for (int i = 0; i < n; i++) {
        uint32_t r = 0;
        if (has) {
            unsigned char ok = 0;
            for (int t = 0; t < 32 && !ok; t++)
                __asm__ volatile ("rdrand %0; setc %1" : "=r"(r), "=qm"(ok) :: "cc");
            if (!ok) has = 0;
        }
        mix = mix * 1664525u + 1013904223u + sys_uptime();
        if (!has) r = mix ^ (mix >> 13);
        seed[i] = (unsigned char)(r ^ (mix >> ((i & 3) * 8)));
    }
}

/* ---- the context ---------------------------------------------------------- */
struct tls_ctx {
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    noanchor_t              na;
    br_sslio_context        io;
    int                     fd;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
};

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    long n = sys_read(fd, buf, (unsigned int)len);
    return (n > 0) ? (int)n : -1;     /* 0 (EOF) / <0 -> error to BearSSL */
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    long n = sys_write(fd, buf, (unsigned int)len);
    return (n > 0) ? (int)n : -1;
}

tls_ctx *tls_open(int fd, const char *host)
{
    tls_ctx *t = (tls_ctx *)malloc(sizeof *t);
    if (!t)
        return 0;
    t->fd = fd;

    br_ssl_client_init_full(&t->sc, &t->xc, 0, 0);     /* no trust anchors */
    t->na.vtable = &noanchor_vtable;
    t->na.inner  = &t->xc.vtable;
    br_ssl_engine_set_x509(&t->sc.eng, &t->na.vtable); /* accept any chain */
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof t->iobuf, 1);

    unsigned char seed[32];
    seed_bytes(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof seed);

    if (!br_ssl_client_reset(&t->sc, host, 0)) {       /* SNI = host */
        free(t);
        return 0;
    }
    br_sslio_init(&t->io, &t->sc.eng, sock_read, &t->fd, sock_write, &t->fd);
    return t;
}

int tls_write(tls_ctx *t, const void *buf, int len)
{
    if (!t) return -1;
    if (br_sslio_write_all(&t->io, buf, (size_t)len) != 0)
        return -1;
    return len;
}

int tls_flush(tls_ctx *t)
{
    if (!t) return -1;
    return (br_sslio_flush(&t->io) == 0) ? 0 : -1;
}

int tls_read(tls_ctx *t, void *buf, int len)
{
    if (!t) return -1;
    return br_sslio_read(&t->io, buf, (size_t)len);    /* <=0 at close/error */
}

void tls_close(tls_ctx *t)
{
    if (t) free(t);
}
