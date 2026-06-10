#ifndef _TLS_H
#define _TLS_H

/*
 * tls.h -- a minimal TLS 1.2 client over a connected TCP socket fd, via BearSSL.
 *
 * This is the ring-3 half of "TLS belongs in userspace": the kernel hands us a
 * plain TCP socket (SYS_SOCKET/CONNECT), and the handshake + record layer run
 * here, on the 32 KB user stack -- a crypto fault just kills the tab, never the
 * kernel.
 *
 * Encrypt-only: certificate chains are accepted WITHOUT anchoring (we ship no
 * CA bundle), so the link is confidential but NOT authenticated.  Fine for a
 * hobby browser fetching public pages; not for anything sensitive.
 */

typedef struct tls_ctx tls_ctx;

/* Wrap an already-connected TCP socket `fd` in TLS, sending `host` as SNI.
 * Returns an opaque context, or NULL on out-of-memory.  The handshake itself
 * runs lazily on the first tls_write/tls_read.  Does not take ownership of fd. */
tls_ctx *tls_open(int fd, const char *host);

/* Write all of buf (len bytes) as application data.  Returns len, or -1. */
int  tls_write(tls_ctx *t, const void *buf, int len);

/* Flush buffered application data onto the wire.  Returns 0 / -1. */
int  tls_flush(tls_ctx *t);

/* Read up to len bytes of application data.  Returns bytes (>0), or <=0 at
 * end-of-stream / on error (a close without close_notify reads as <=0). */
int  tls_read(tls_ctx *t, void *buf, int len);

/* BearSSL's last engine error code (BR_ERR_*), for diagnostics.  0 = none. */
int  tls_error(tls_ctx *t);

/* Free the context (does NOT close the underlying fd). */
void tls_close(tls_ctx *t);

#endif /* _TLS_H */
