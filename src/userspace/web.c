/*
 * web.c -- minimal HTTP/1.1 client in userspace, over kernel TCP sockets.
 *
 * Ported from the in-kernel wget (shell_cmd_net.c): same URL parse, header
 * scan, chunked decode and 3xx redirect following -- but the transport is now a
 * ring-3 socket (SYS_SOCKET/CONNECT), plain for http:// and BearSSL (tls.c) for
 * https://.  HTTP is application-layer and no longer belongs in the kernel.
 */

#include "web.h"
#include "tls.h"
#include "syscall.h"
#include <stdlib.h>
#include <string.h>

#define WEB_MAX_BYTES (8u * 1024u * 1024u)

/* --- temporary diagnostics to COM1 (serial only; invisible in the GUI) ----
 * Pinpoints where a fetch dies (DNS / connect / TLS).  Remove once verified. */
static void wdbg(const char *s) { unsigned n = 0; while (s[n]) n++; (void)syscall2(SYS_WRITE_SERIAL, (long)s, (long)n); }
static void wdbgn(unsigned v) {
    char b[12]; int i = 0;
    if (!v) { wdbg("0"); return; }
    while (v) { b[i++] = (char)('0' + v % 10u); v /= 10u; }
    char o[12]; for (int j = 0; j < i; j++) o[j] = b[i - 1 - j];
    (void)syscall2(SYS_WRITE_SERIAL, (long)o, (long)i);
}

/* scheme://host[:port][/path] -> pieces.  Returns 0, or -1 on a malformed URL. */
static int parse_url(const char *url, char *host, unsigned hostcap,
                     unsigned short *port, char *path, unsigned pathcap, int *tls)
{
    const char *p = url;
    *tls = 0;
    if (!strncmp(p, "https://", 8)) { p += 8; *tls = 1; }
    else if (!strncmp(p, "http://", 7)) p += 7;
    /* no scheme -> treat as http:// from the start */

    unsigned hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < hostcap - 1) host[hi++] = *p++;
    host[hi] = 0;
    if (hi == 0) return -1;

    *port = *tls ? 443 : 80;
    if (*p == ':') {
        p++;
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10u + (unsigned)(*p - '0'); p++; }
        if (v == 0 || v > 65535u) return -1;
        *port = (unsigned short)v;
    }
    unsigned pi = 0;
    if (*p != '/') path[pi++] = '/';
    while (*p && pi < pathcap - 1) path[pi++] = *p++;
    path[pi] = 0;
    return 0;
}

/* case-insensitive search of needle in the first hlen bytes of hay */
static const char *hdr_find(const char *hay, unsigned hlen, const char *needle)
{
    unsigned nlen = (unsigned)strlen(needle);
    if (nlen == 0 || nlen > hlen) return 0;
    for (unsigned i = 0; i + nlen <= hlen; i++) {
        unsigned j = 0;
        for (; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nlen) return hay + i;
    }
    return 0;
}

/* decode chunked transfer-encoding in place; returns new length or (unsigned)-1 */
static unsigned dechunk(unsigned char *body, unsigned len)
{
    unsigned rd = 0, wr = 0;
    while (rd < len) {
        unsigned sz = 0; int any = 0;
        while (rd < len && body[rd] != '\r' && body[rd] != ';') {
            char c = (char)body[rd++]; unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return (unsigned)-1;
            sz = (sz << 4) | d; any = 1;
        }
        if (!any) return (unsigned)-1;
        while (rd < len && body[rd] != '\n') rd++;
        if (rd < len) rd++;
        if (sz == 0) break;
        if (rd + sz > len) return (unsigned)-1;
        memmove(body + wr, body + rd, sz);
        wr += sz; rd += sz;
        if (rd + 2 <= len && body[rd] == '\r' && body[rd + 1] == '\n') rd += 2;
    }
    return wr;
}

/* resolve a redirect ref (absolute / //host / /path / relative) against base */
static void resolve_redirect(const char *base, const char *ref, char *out, unsigned outcap)
{
    out[0] = 0;
    int isabs = 0;
    for (const char *p = ref; *p && p < ref + 12; p++) {
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') { isabs = 1; break; }
        if (*p == '/') break;
    }
    if (isabs) { unsigned i = 0; while (ref[i] && i < outcap - 1) { out[i] = ref[i]; i++; } out[i] = 0; return; }

    char scheme[8] = {0}, host[128] = {0};
    {
        const char *p = base; unsigned i = 0;
        while (*p && *p != ':' && i < 7) scheme[i++] = *p++;
        scheme[i] = 0;
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') p += 3; else p = base;
        i = 0;
        while (*p && *p != '/' && i < 127) host[i++] = *p++;
        host[i] = 0;
    }

    char *o = out; const char *lim = out + outcap - 1;
    if (ref[0] == '/' && ref[1] == '/') {
        for (const char *p = scheme; *p && o < lim;) *o++ = *p++;
        if (o < lim) *o++ = ':';
        for (const char *p = ref; *p && o < lim;) *o++ = *p++;
    } else if (ref[0] == '/') {
        for (const char *p = scheme; *p && o < lim;) *o++ = *p++;
        for (const char *p = "://"; *p && o < lim;) *o++ = *p++;
        for (const char *p = host; *p && o < lim;) *o++ = *p++;
        for (const char *p = ref; *p && o < lim;) *o++ = *p++;
    } else {
        for (const char *p = scheme; *p && o < lim;) *o++ = *p++;
        for (const char *p = "://"; *p && o < lim;) *o++ = *p++;
        for (const char *p = host; *p && o < lim;) *o++ = *p++;
        if (o < lim) *o++ = '/';
        for (const char *p = ref; *p && o < lim;) *o++ = *p++;
    }
    *o = 0;
}

static void redirect_loc(const unsigned char *buf, unsigned hdr_end, const char *base,
                         char *out, unsigned outcap)
{
    out[0] = 0;
    const char *h = hdr_find((const char *)buf, hdr_end, "\nlocation:");
    if (!h) return;
    h += 10;
    while (*h == ' ' || *h == '\t') h++;
    char loc[1024]; unsigned li = 0;
    while (*h && *h != '\r' && *h != '\n' && li < sizeof(loc) - 1) loc[li++] = *h++;
    loc[li] = 0;
    if (loc[0]) resolve_redirect(base, loc, out, outcap);
}

/* ---- transport: plain socket or TLS -------------------------------------- */
typedef struct { int fd; tls_ctx *tls; } conn_t;

static int conn_send(conn_t *c, const char *buf, int len)
{
    if (c->tls) {
        if (tls_write(c->tls, buf, len) != len) return -1;
        return tls_flush(c->tls);
    }
    int left = len; const char *p = buf;
    while (left > 0) { long n = sys_write(c->fd, p, (unsigned)left); if (n <= 0) return -1; p += n; left -= (int)n; }
    return 0;
}
static int conn_recv(conn_t *c, unsigned char *buf, int len)
{
    if (c->tls) return tls_read(c->tls, buf, len);
    return (int)sys_read(c->fd, buf, (unsigned)len);   /* 0 = EOF */
}

/* one fetch; on success: malloc'd body (caller frees) + len/status/loc */
static int fetch_once(const char *url, unsigned char **out_body, unsigned *out_len,
                      int *out_status, char *loc_out, unsigned loc_cap)
{
    *out_body = 0; *out_len = 0; *out_status = 0; if (loc_out) loc_out[0] = 0;

    char host[128], path[1024];
    unsigned short port; int tls;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path, &tls) != 0) return -1;

    wdbg("[web] fetch host="); wdbg(host); wdbg(" port="); wdbgn(port); wdbg(tls ? " https\n" : " http\n");
    unsigned char ip[4];
    if (sys_resolve(host, ip) != 0) { wdbg("[web] DNS resolve FAILED (network up? nic attached?)\n"); return -1; }
    wdbg("[web] dns ok ip="); wdbgn(ip[0]); wdbg("."); wdbgn(ip[1]); wdbg("."); wdbgn(ip[2]); wdbg("."); wdbgn(ip[3]); wdbg("\n");

    int fd = sys_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { wdbg("[web] socket() FAILED\n"); return -1; }
    struct sockaddr_in sa;
    sa.sin_family      = AF_INET;
    sa.sin_port        = mk_htons(port);
    sa.sin_addr.s_addr = (unsigned)ip[0] | ((unsigned)ip[1] << 8) | ((unsigned)ip[2] << 16) | ((unsigned)ip[3] << 24);
    for (int i = 0; i < 8; i++) sa.sin_zero[i] = 0;
    if (sys_connect(fd, &sa, (int)sizeof sa) != 0) { wdbg("[web] connect() FAILED\n"); sys_close(fd); return -1; }
    wdbg("[web] tcp connected\n");

    conn_t c; c.fd = fd; c.tls = 0;
    if (tls) {
        c.tls = tls_open(fd, host);
        if (!c.tls) { wdbg("[web] tls_open FAILED (oom / reset)\n"); sys_close(fd); return -1; }
        wdbg("[web] tls context ready (handshake on first write)\n");
    }

    char req[1280]; unsigned rl = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host,
        "\r\nUser-Agent: makar-mxweb\r\nAccept: */*\r\nConnection: close\r\n\r\n" };
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const char *s = parts[i];
        while (*s && rl < sizeof(req) - 1) req[rl++] = *s++;
    }
    if (conn_send(&c, req, (int)rl) != 0) {
        wdbg("[web] request send FAILED");
        if (c.tls) { wdbg(" tls_err="); wdbgn((unsigned)tls_error(c.tls)); tls_close(c.tls); }
        wdbg("\n");
        sys_close(fd); return -1;
    }
    wdbg("[web] request sent; reading response\n");

    unsigned cap = 16384, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { if (c.tls) tls_close(c.tls); sys_close(fd); return -1; }
    for (;;) {
        if (len + 4096 > cap) {
            unsigned ncap = cap * 2;
            if (ncap > WEB_MAX_BYTES) ncap = WEB_MAX_BYTES;
            if (ncap <= cap) break;                 /* hit the size cap */
            unsigned char *nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) break;
            buf = nb; cap = ncap;
        }
        int n = conn_recv(&c, buf + len, (int)(cap - len));
        if (n <= 0) break;
        len += (unsigned)n;
    }
    wdbg("[web] received bytes="); wdbgn(len);
    if (c.tls) { wdbg(" tls_err="); wdbgn((unsigned)tls_error(c.tls)); }
    wdbg("\n");
    if (c.tls) tls_close(c.tls);
    sys_close(fd);

    if (len == 0) { free(buf); return -1; }

    unsigned hdr_end = 0;
    for (unsigned i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') { hdr_end = i + 4; break; }
    if (hdr_end == 0) { free(buf); return -1; }

    int status = 0;
    { const char *sp = (const char *)buf, *q = sp;
      while ((unsigned)(q - sp) < hdr_end && *q != ' ') q++;
      if (*q == ' ') { q++; for (int i = 0; i < 3 && q[i] >= '0' && q[i] <= '9'; i++) status = status * 10 + (q[i] - '0'); } }
    *out_status = status;
    if (loc_out && status >= 300 && status < 400) redirect_loc(buf, hdr_end, url, loc_out, loc_cap);

    int chunked = hdr_find((const char *)buf, hdr_end, "Transfer-Encoding: chunked") != 0;
    unsigned body_len = len - hdr_end;
    memmove(buf, buf + hdr_end, body_len);
    if (chunked) { unsigned dl = dechunk(buf, body_len); if (dl == (unsigned)-1) { free(buf); return -1; } body_len = dl; }

    *out_body = buf; *out_len = body_len;
    return 0;
}

int web_fetch(const char *url, const char *outpath)
{
    char cur[1024]; unsigned ci = 0;
    while (url[ci] && ci < sizeof(cur) - 1) { cur[ci] = url[ci]; ci++; }
    cur[ci] = 0;

    for (int hop = 0; hop < 6; hop++) {
        unsigned char *body = 0; unsigned blen = 0; int status = 0; char loc[1024];
        int rc = fetch_once(cur, &body, &blen, &status, loc, sizeof loc);
        if (rc != 0) return -1;
        if (loc[0] && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            free(body);
            unsigned i = 0; while (loc[i] && i < sizeof(cur) - 1) { cur[i] = loc[i]; i++; } cur[i] = 0;
            continue;
        }
        if (status < 200 || status >= 300) { free(body); return -(status ? status : 1); }
        int wr = sys_write_file(outpath, body, blen);
        free(body);
        return (wr == 0) ? (int)blen : -2;
    }
    return -1;   /* too many redirects */
}
