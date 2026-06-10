/*
 * shell_cmd_net.c -- network client shell commands.
 *
 * wget: fetch an http:// URL over lwIP TCP and save the body to the VFS.
 *       Plain HTTP only (no TLS); resolves the host via lwIP DNS, issues a
 *       single HTTP/1.1 GET with Connection: close, de-chunks if needed, and
 *       writes the body to a writable backend (tmpfs/FAT32/ext2).
 *
 * The network/parse core lives in wget_fetch() (declared in <kernel/wget.h>)
 * so the ktest net section can exercise it headlessly against a slirp
 * guestfwd HTTP fixture.
 */
#include "shell_priv.h"
#include <kernel/tty.h>
#include <kernel/heap.h>
#include <kernel/timer.h>
#include <kernel/task.h>
#include <kernel/vfs.h>
#include <kernel/wget.h>
#include <kernel/net_lwip.h>
#include <lwip/tcp.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <kernel/serial.h>
#include "bearssl.h"

#define WGET_MAX_BYTES   (8u * 1024u * 1024u)   /* hard cap on a download    */
#define WGET_CONNECT_TMO 500u                   /* ticks (100 Hz) to connect */
#define WGET_TLS_TMO     1500u                  /* per-call stall cap in the TLS
                                                 * I/O callbacks (~15 s): fail fast
                                                 * and diagnosably, never wedge. */
#define WGET_IDLE_TMO    6000u                  /* ticks with no new data (60s):
                                                 * generous so TCP retransmits of
                                                 * a multi-MB transfer's tail
                                                 * complete before we give up. */

typedef struct {
    uint8_t *buf;
    uint32_t len;
    uint32_t cap;
    int connected;
    int done;       /* remote closed cleanly */
    int err;
    int aborted;    /* tcp_err fired -> lwIP already freed the pcb */
    int tls;        /* https: route recv into rx (ciphertext)      */
    struct tcp_pcb *pcb;                /* for the TLS write callback   */
    uint8_t *rx; uint32_t rxlen, rxcap, rxrd;   /* raw ciphertext recv ring */
} wget_state_t;

static int wget_sink(wget_state_t *s, const uint8_t *data, uint32_t n)
{
    if (s->len + n > WGET_MAX_BYTES) { s->err = 1; return -1; }
    if (s->len + n > s->cap) {
        uint32_t ncap = s->cap ? s->cap : 8192u;
        while (ncap < s->len + n) ncap <<= 1;
        if (ncap > WGET_MAX_BYTES) ncap = WGET_MAX_BYTES;
        uint8_t *nb = krealloc(s->buf, ncap);
        if (!nb) { s->err = 1; return -1; }
        s->buf = nb;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, data, n);
    s->len += n;
    return 0;
}

/* Append raw (ciphertext) bytes to the TLS recv ring. */
static int wget_rx_push(wget_state_t *s, const uint8_t *data, uint32_t n)
{
    if (s->rxlen + n > s->rxcap) {
        uint32_t ncap = s->rxcap ? s->rxcap : 16384u;
        while (ncap < s->rxlen + n) ncap <<= 1;
        if (ncap > WGET_MAX_BYTES) ncap = WGET_MAX_BYTES;
        uint8_t *nb = krealloc(s->rx, ncap);
        if (!nb) { s->err = 1; return -1; }
        s->rx = nb; s->rxcap = ncap;
    }
    memcpy(s->rx + s->rxlen, data, n);
    s->rxlen += n;
    return 0;
}

static err_t wget_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    wget_state_t *s = (wget_state_t *)arg;
    if (err != ERR_OK) {
        s->err = 1;
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p) {                    /* FIN: remote closed */
        s->done = 1;
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        if (s->tls) wget_rx_push(s, (const uint8_t *)q->payload, q->len);
        else        wget_sink(s, (const uint8_t *)q->payload, q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t wget_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    wget_state_t *s = (wget_state_t *)arg;
    if (err != ERR_OK) {
        s->err = 1;
        return ERR_OK;
    }
    s->connected = 1;
    tcp_recv(pcb, wget_recv);
    return ERR_OK;
}

static void wget_err_cb(void *arg, err_t err)
{
    (void)err;
    wget_state_t *s = (wget_state_t *)arg;
    /* lwIP has already freed the pcb by the time this fires; mark it so the
     * caller never touches the dangling pointer (tcp_abort/tcp_close = UAF). */
    if (s) { s->err = 1; s->aborted = 1; }
}

/* Parse "http://host[:port][/path]" into pieces.  Returns 0 on success,
 * -2 for an https:// URL (TLS unsupported), -1 for any other malformed URL. */
static int wget_parse_url(const char *url, char *host, uint32_t host_cap,
                          uint16_t *port, char *path, uint32_t path_cap, int *tls)
{
    const char *p = url;
    *tls = 0;
    if (strncmp(p, "https://", 8) == 0) { p += 8; *tls = 1; }
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    /* no scheme -> treat as http://, parse host from the start */

    uint32_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < host_cap - 1)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0)
        return -1;

    *port = *tls ? 443 : 80;
    if (*p == ':') {
        p++;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
        if (v == 0 || v > 65535u)
            return -1;
        *port = (uint16_t)v;
    }

    uint32_t pi = 0;
    if (*p != '/')
        path[pi++] = '/';
    while (*p && pi < path_cap - 1)
        path[pi++] = *p++;
    path[pi] = '\0';
    return 0;
}

/* Case-insensitive search for needle within the first hlen bytes of hay. */
static const char *wget_hdr_find(const char *hay, uint32_t hlen, const char *needle)
{
    uint32_t nlen = (uint32_t)strlen(needle);
    if (nlen == 0 || nlen > hlen)
        return NULL;
    for (uint32_t i = 0; i + nlen <= hlen; i++) {
        uint32_t j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nlen)
            return hay + i;
    }
    return NULL;
}

/* Decode chunked transfer-encoding in place.  Returns new length, or -1. */
static uint32_t wget_dechunk(uint8_t *body, uint32_t len)
{
    uint32_t rd = 0, wr = 0;
    while (rd < len) {
        uint32_t sz = 0;
        int any = 0;
        while (rd < len && body[rd] != '\r' && body[rd] != ';') {
            char c = (char)body[rd++];
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return (uint32_t)-1;
            sz = (sz << 4) | d;
            any = 1;
        }
        if (!any) return (uint32_t)-1;
        while (rd < len && body[rd] != '\n') rd++;
        if (rd < len) rd++;
        if (sz == 0) break;
        if (rd + sz > len) return (uint32_t)-1;
        memmove(body + wr, body + rd, sz);
        wr += sz;
        rd += sz;
        if (rd + 2 <= len && body[rd] == '\r' && body[rd + 1] == '\n') rd += 2;
    }
    return wr;
}

/* ===== HTTPS over BearSSL ================================================
 * A TLS-1.2 client over the same raw-lwIP poll loop wget uses for HTTP.  We
 * ship no CA bundle, so certificate chains are accepted without anchoring (the
 * x509-no-anchor wrapper below): this gets the link *encrypted* but
 * authenticates nothing -- fine for a hobby browser fetching public pages, not
 * for anything sensitive.  SNI carries the host name (so CloudFlare / name
 * vhosts answer); the DRBG is seeded from RDRAND mixed with the timer. */
typedef struct { const br_x509_class *vtable; const br_x509_class **inner; } x509noanchor_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *sn)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; (*x->inner)->start_chain(x->inner, sn); }
static void xwc_start_cert(const br_x509_class **ctx, uint32_t len)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; (*x->inner)->start_cert(x->inner, len); }
static void xwc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; (*x->inner)->append(x->inner, buf, len); }
static void xwc_end_cert(const br_x509_class **ctx)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; (*x->inner)->end_cert(x->inner); }
static unsigned xwc_end_chain(const br_x509_class **ctx)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; unsigned r=(*x->inner)->end_chain(x->inner); return (r==BR_ERR_X509_NOT_TRUSTED)?0:r; }
static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{ x509noanchor_context *x=(x509noanchor_context*)(void*)ctx; return (*x->inner)->get_pkey(x->inner, usages); }
static const br_x509_class x509noanchor_vtable = {
    sizeof(x509noanchor_context),
    xwc_start_chain, xwc_start_cert, xwc_append, xwc_end_cert, xwc_end_chain, xwc_get_pkey
};

static int cpu_has_rdrand(void)
{
    uint32_t eax, ecx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=c"(ecx) : "a"(1) : "ebx", "edx");
    (void)eax;
    return (ecx >> 30) & 1u;
}

/* 32-byte DRBG seed.  RDRAND only when CPUID advertises it (so we never execute
 * an unsupported opcode -> #UD -> kernel fault), mixed with the timer; otherwise
 * a timer-seeded LCG -- weak, enough to complete a hobby-browser handshake, NOT
 * for anything sensitive. */
static void wget_tls_seed(unsigned char *seed, int n)
{
    int has = cpu_has_rdrand();
    uint32_t mix = timer_get_ticks();
    for (int i = 0; i < n; i++) {
        uint32_t r = 0;
        if (has) {
            unsigned char ok = 0;
            for (int t = 0; t < 32 && !ok; t++)
                __asm__ volatile ("rdrand %0; setc %1" : "=r"(r), "=qm"(ok) :: "cc");
            if (!ok) has = 0;
        }
        mix = mix * 1664525u + 1013904223u + timer_get_ticks();
        if (!has) r = mix ^ (mix >> 13);
        seed[i] = (unsigned char)(r ^ (mix >> ((i & 3) * 8)));
    }
}

static int wget_tls_read(void *ctx, unsigned char *buf, size_t len)
{
    wget_state_t *s = (wget_state_t *)ctx;
    uint32_t t0 = timer_get_ticks();
    while (s->rxrd >= s->rxlen) {
        if (s->done || s->err || s->aborted) return -1;
        if (timer_get_ticks() - t0 > WGET_TLS_TMO) return -1;
        net_lwip_poll();
        task_yield();
    }
    uint32_t avail = s->rxlen - s->rxrd;
    uint32_t n = (len < avail) ? (uint32_t)len : avail;
    memcpy(buf, s->rx + s->rxrd, n);
    s->rxrd += n;
    if (s->rxrd == s->rxlen) { s->rxlen = 0; s->rxrd = 0; }
    return (int)n;
}

static int wget_tls_write(void *ctx, const unsigned char *buf, size_t len)
{
    wget_state_t *s = (wget_state_t *)ctx;
    uint32_t t0 = timer_get_ticks();
    for (;;) {
        if (s->err || s->aborted) return -1;
        u16_t sb = tcp_sndbuf(s->pcb);
        if (sb > 0) {
            uint32_t n = (len < sb) ? (uint32_t)len : sb;
            if (tcp_write(s->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return -1;
            tcp_output(s->pcb);
            return (int)n;
        }
        if (timer_get_ticks() - t0 > WGET_TLS_TMO) return -1;
        net_lwip_poll();
        task_yield();
    }
}

/* Handshake + one HTTP request over TLS; decrypted response accumulates into
 * s->buf (so the caller's header-parse / de-chunk path is unchanged). */
static int wget_tls(wget_state_t *s, const char *host, const char *req, uint32_t reqlen)
{
    br_ssl_client_context  *sc  = krealloc(NULL, sizeof *sc);
    br_x509_minimal_context *xc = krealloc(NULL, sizeof *xc);
    x509noanchor_context   *xwc = krealloc(NULL, sizeof *xwc);
    unsigned char *iobuf = krealloc(NULL, BR_SSL_BUFSIZE_BIDI);
    int rc = -1;
    if (!sc || !xc || !xwc || !iobuf) { KLOG("[TLS] oom\n"); goto out; }
    KLOG("[TLS] handshake "); KLOG((char *)host); KLOG("\n");

    br_ssl_client_init_full(sc, xc, NULL, 0);            /* no trust anchors */
    xwc->vtable = &x509noanchor_vtable; xwc->inner = &xc->vtable;
    br_ssl_engine_set_x509(&sc->eng, &xwc->vtable);      /* accept any chain */
    br_ssl_engine_set_buffer(&sc->eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    { unsigned char seed[32]; wget_tls_seed(seed, sizeof seed);
      br_ssl_engine_inject_entropy(&sc->eng, seed, sizeof seed); }
    if (!br_ssl_client_reset(sc, host, 0)) goto out;

    br_sslio_context ioc;
    br_sslio_init(&ioc, &sc->eng, wget_tls_read, s, wget_tls_write, s);
    if (br_sslio_write_all(&ioc, req, reqlen) != 0) {
        KLOG("[TLS] write_all failed err="); KLOG_HEX((uint32_t)br_ssl_engine_last_error(&sc->eng));
        KLOG(" rx="); KLOG_HEX(s->rxlen); KLOG(" serr="); KLOG_HEX((uint32_t)s->err); KLOG("\n");
        goto out;
    }
    br_sslio_flush(&ioc);
    for (;;) {
        unsigned char tmp[1500];
        int n = br_sslio_read(&ioc, tmp, sizeof tmp);
        if (n <= 0) break;
        if (wget_sink(s, tmp, (uint32_t)n) < 0) break;
    }
    KLOG("[TLS] done err="); KLOG_HEX((uint32_t)br_ssl_engine_last_error(&sc->eng));
    KLOG(" body="); KLOG_HEX(s->len); KLOG("\n");
    rc = (s->len > 0) ? 0 : -1;     /* a close without close_notify is OK */
out:
    kfree(sc); kfree(xc); kfree(xwc); kfree(iobuf);
    return rc;
}

/* Resolve a redirect target (absolute / //host / /path / relative) vs base. */
static void wget_resolve(const char *base, const char *ref, char *out, uint32_t outcap)
{
    out[0] = 0;
    int abs = 0;
    for (const char *p = ref; *p && p < ref + 12; p++) { if (p[0]==':'&&p[1]=='/'&&p[2]=='/') { abs=1; break; } if (*p=='/') break; }
    if (abs) { uint32_t i=0; while (ref[i] && i<outcap-1){ out[i]=ref[i]; i++; } out[i]=0; return; }
    char scheme[8]={0}, host[128]={0};
    { const char *p=base; uint32_t i=0; while (*p && *p!=':' && i<7) scheme[i++]=*p++; scheme[i]=0;
      if (p[0]==':'&&p[1]=='/'&&p[2]=='/') p+=3; else p=base;
      i=0; while (*p && *p!='/' && i<127) host[i++]=*p++; host[i]=0; }
    char *o=out; const char *lim=out+outcap-1;
    if (ref[0]=='/'&&ref[1]=='/') { for(const char*p=scheme;*p&&o<lim;)*o++=*p++; if(o<lim)*o++=':'; for(const char*p=ref;*p&&o<lim;)*o++=*p++; }
    else if (ref[0]=='/') { for(const char*p=scheme;*p&&o<lim;)*o++=*p++; for(const char*p="://";*p&&o<lim;)*o++=*p++; for(const char*p=host;*p&&o<lim;)*o++=*p++; for(const char*p=ref;*p&&o<lim;)*o++=*p++; }
    else { for(const char*p=scheme;*p&&o<lim;)*o++=*p++; for(const char*p="://";*p&&o<lim;)*o++=*p++; for(const char*p=host;*p&&o<lim;)*o++=*p++; if(o<lim)*o++='/'; for(const char*p=ref;*p&&o<lim;)*o++=*p++; }
    *o=0;
}

/* Pull the Location header out of a 3xx response and resolve it vs `base`. */
static void wget_redirect_loc(const uint8_t *buf, uint32_t hdr_end, const char *base,
                              char *out, uint32_t outcap)
{
    out[0] = 0;
    const char *h = wget_hdr_find((const char *)buf, hdr_end, "\nlocation:");
    if (!h) return;
    h += 10;
    while (*h==' ' || *h=='\t') h++;
    char loc[1024]; uint32_t li=0;
    while (*h && *h!='\r' && *h!='\n' && li<sizeof(loc)-1) loc[li++]=*h++;
    loc[li]=0;
    if (loc[0]) wget_resolve(base, loc, out, outcap);
}

static int wget_fetch_once(const char *url, uint8_t **out_body, uint32_t *out_len,
                           int *out_status, char *loc_out, uint32_t loc_cap)
{
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;
    if (loc_out) loc_out[0] = 0;

    char host[128], path[512];
    uint16_t port = 80; int tls = 0;
    if (wget_parse_url(url, host, sizeof(host), &port, path, sizeof(path), &tls) != 0)
        return -1;

    if (net_lwip_init() != 0 || !net_lwip_ready())
        return -1;

    uint8_t ip[4];
    if (net_lwip_resolve(host, ip, 400) != 0)
        return -1;

    wget_state_t st;
    memset(&st, 0, sizeof st);
    st.tls = tls;

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return -1;
    tcp_arg(pcb, &st);
    tcp_err(pcb, wget_err_cb);

    ip_addr_t dst;
    IP_ADDR4(&dst, ip[0], ip[1], ip[2], ip[3]);
    if (tcp_connect(pcb, &dst, port, wget_connected) != ERR_OK) {
        tcp_abort(pcb);
        return -1;
    }

    uint32_t t0 = timer_get_ticks();
    while (timer_get_ticks() - t0 < WGET_CONNECT_TMO && !st.connected && !st.err) {
        net_lwip_poll();
        task_yield();
    }
    if (!st.connected || st.err) {
        if (!st.aborted) tcp_abort(pcb);   /* skip if lwIP already freed it */
        kfree(st.buf);
        return -1;
    }

    char req[768];
    uint32_t rl = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host,
                            "\r\nUser-Agent: makar-wget\r\nAccept: */*\r\n"
                            "Connection: close\r\n\r\n" };
    for (uint32_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const char *s = parts[i];
        while (*s && rl < sizeof(req) - 1) req[rl++] = *s++;
    }
    if (st.tls) {
        /* In-kernel BearSSL overflowed the 8 KB ring-0 task stack (cert-chain
         * parse + RSA/EC bignum) -> TLS is moving to userspace.  Don't run it
         * here; fail cleanly so https can never wedge or fault the kernel. */
        (void)wget_tls;
        if (!st.aborted) tcp_abort(pcb);
        kfree(st.buf); kfree(st.rx);
        return -1;
    } else {
        if (tcp_write(pcb, req, (u16_t)rl, TCP_WRITE_FLAG_COPY) != ERR_OK ||
            tcp_output(pcb) != ERR_OK) {
            if (!st.aborted) tcp_abort(pcb);
            kfree(st.buf);
            return -1;
        }
        uint32_t last_len = 0;
        uint32_t last_progress = timer_get_ticks();
        while (!st.done && !st.err) {
            net_lwip_poll();
            task_yield();
            if (st.len != last_len) { last_len = st.len; last_progress = timer_get_ticks(); }
            else if (timer_get_ticks() - last_progress > WGET_IDLE_TMO) break;
        }
    }
    /* Only touch the pcb if lwIP hasn't already freed it via tcp_err.  If
     * tcp_close can't proceed (out of memory), fall back to abort. */
    if (!st.aborted) {
        tcp_recv(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
    }
    kfree(st.rx); st.rx = NULL;

    if (st.len == 0) { kfree(st.buf); return -1; }

    uint32_t hdr_end = 0;
    for (uint32_t i = 0; i + 3 < st.len; i++) {
        if (st.buf[i] == '\r' && st.buf[i + 1] == '\n' &&
            st.buf[i + 2] == '\r' && st.buf[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end == 0) { kfree(st.buf); return -1; }

    int status = 0;
    {
        const char *sp = (const char *)st.buf;
        const char *q = sp;
        while ((uint32_t)(q - sp) < hdr_end && *q != ' ') q++;
        if (*q == ' ') {
            q++;
            for (int i = 0; i < 3 && q[i] >= '0' && q[i] <= '9'; i++)
                status = status * 10 + (q[i] - '0');
        }
    }
    if (out_status) *out_status = status;
    if (loc_out && status >= 300 && status < 400)
        wget_redirect_loc(st.buf, hdr_end, url, loc_out, loc_cap);

    int chunked = wget_hdr_find((const char *)st.buf, hdr_end,
                                "Transfer-Encoding: chunked") != NULL;

    /* Slide the body to the front of the allocation so the caller owns one
     * buffer that starts at the body. */
    uint32_t body_len = st.len - hdr_end;
    memmove(st.buf, st.buf + hdr_end, body_len);

    if (chunked) {
        uint32_t dl = wget_dechunk(st.buf, body_len);
        if (dl == (uint32_t)-1) { kfree(st.buf); return -1; }
        body_len = dl;
    }

    if (out_body) *out_body = st.buf;
    else kfree(st.buf);
    if (out_len) *out_len = body_len;
    return 0;
}

/* Public entry: fetch `url`, following HTTP 3xx redirects (http->https etc.),
 * up to a small hop cap. */
int wget_fetch(const char *url, uint8_t **out_body, uint32_t *out_len, int *out_status)
{
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;

    char cur[1024]; uint32_t ci=0;
    while (url[ci] && ci<sizeof(cur)-1) { cur[ci]=url[ci]; ci++; }
    cur[ci]=0;

    for (int hop = 0; hop < 6; hop++) {
        uint8_t *body = NULL; uint32_t blen = 0; int status = 0; char loc[1024];
        int rc = wget_fetch_once(cur, &body, &blen, &status, loc, sizeof loc);
        if (rc != 0) return rc;
        if (loc[0] && (status==301||status==302||status==303||status==307||status==308)) {
            if (body) kfree(body);
            uint32_t i=0; while (loc[i] && i<sizeof(cur)-1){ cur[i]=loc[i]; i++; } cur[i]=0;
            continue;
        }
        if (out_body) *out_body = body; else if (body) kfree(body);
        if (out_len) *out_len = blen;
        if (out_status) *out_status = status;
        return 0;
    }
    return -1;   /* too many redirects */
}

static void cmd_wget(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: wget http://host[:port]/path [outfile]\n");
        return;
    }

    /* Pre-validate the URL so we can give a clear https/TLS message. */
    char host[128], path[512];
    uint16_t port = 80; int tls = 0;
    int pr = wget_parse_url(argv[1], host, sizeof(host), &port, path, sizeof(path), &tls);
    if (pr != 0) {
        t_writestring("wget: malformed URL (expected http[s]://host[:port]/path)\n");
        return;
    }

    /* Output path: explicit arg, else /tmp/<basename of path>. */
    char outpath[VFS_PATH_MAX];
    if (argc >= 3) {
        strncpy(outpath, argv[2], sizeof(outpath) - 1);
        outpath[sizeof(outpath) - 1] = '\0';
    } else {
        const char *base = path;
        for (const char *c = path; *c; c++)
            if (*c == '/') base = c + 1;
        if (!*base)
            base = "index.html";
        uint32_t o = 0;
        const char *pre = "/tmp/";
        while (*pre && o < sizeof(outpath) - 1) outpath[o++] = *pre++;
        while (*base && o < sizeof(outpath) - 1) outpath[o++] = *base++;
        outpath[o] = '\0';
    }

    t_writestring("Fetching "); t_writestring(argv[1]); t_writestring("\n");

    uint8_t *body = NULL;
    uint32_t body_len = 0;
    int status = 0;
    int rc = wget_fetch(argv[1], &body, &body_len, &status);
    if (rc != 0) {
        t_writestring("wget: fetch failed (DNS/connect/response error; network up?)\n");
        return;
    }

    t_writestring("HTTP status: "); t_dec((uint32_t)status); t_writestring("\n");
    if (status < 200 || status >= 300) {
        t_writestring("wget: non-2xx status, not saving "
                      "(redirects/https not followed)\n");
        kfree(body);
        return;
    }

    if (vfs_write_file(outpath, body, body_len) != 0) {
        t_writestring("wget: write failed (read-only path? use /tmp or a mounted disk)\n");
        kfree(body);
        return;
    }

    t_writestring("Saved ");
    t_dec(body_len);
    t_writestring(" bytes to ");
    t_writestring(outpath);
    t_writestring("\n");
    kfree(body);
}

const shell_cmd_entry_t net_cmds[] = {
    { "wget", cmd_wget },
    { NULL, NULL }
};
