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

#define WGET_MAX_BYTES   (8u * 1024u * 1024u)   /* hard cap on a download    */
#define WGET_CONNECT_TMO 500u                   /* ticks (100 Hz) to connect */
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
    for (struct pbuf *q = p; q; q = q->next)
        wget_sink(s, (const uint8_t *)q->payload, q->len);
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
                          uint16_t *port, char *path, uint32_t path_cap)
{
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        return -2;

    uint32_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < host_cap - 1)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0)
        return -1;

    *port = 80;
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

int wget_fetch(const char *url, uint8_t **out_body, uint32_t *out_len,
               int *out_status)
{
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;

    char host[128], path[512];
    uint16_t port = 80;
    if (wget_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        return -1;

    if (net_lwip_init() != 0 || !net_lwip_ready())
        return -1;

    uint8_t ip[4];
    if (net_lwip_resolve(host, ip, 400) != 0)
        return -1;

    wget_state_t st;
    memset(&st, 0, sizeof st);

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
    /* Only touch the pcb if lwIP hasn't already freed it via tcp_err.  If
     * tcp_close can't proceed (out of memory), fall back to abort. */
    if (!st.aborted) {
        tcp_recv(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK)
            tcp_abort(pcb);
    }

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

static void cmd_wget(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: wget http://host[:port]/path [outfile]\n");
        return;
    }

    /* Pre-validate the URL so we can give a clear https/TLS message. */
    char host[128], path[512];
    uint16_t port = 80;
    int pr = wget_parse_url(argv[1], host, sizeof(host), &port, path, sizeof(path));
    if (pr == -2) {
        t_writestring("wget: https:// not supported (no TLS); use http://\n");
        return;
    }
    if (pr != 0) {
        t_writestring("wget: malformed URL (expected http://host[:port]/path)\n");
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
