/*
 * makx.c -- client side of the Makar display-server protocol (see makx.h).
 *
 * Thin wrappers over sys_ipc_sendrec + sys_surface_map.  A client renders into
 * the shared surface and uses these to (a) establish a window, (b) drain input
 * the server forwards, and (c) flush completed frames.  The server replies to
 * every request with at most one queued input event plus a "still pending"
 * count, so a client drains its input by polling until the count reaches zero
 * -- no unsolicited server->client sends, which keeps the synchronous IPC
 * rendezvous simple (the server is purely reactive to client requests).
 */
#include "syscall.h"
#include "makx.h"

static int mx_atoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static int mx_streq(const char *a, const char *b)
{
    int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == b[i];
}

/* Fold one reply event into the connection state.  Keys are buffered; mouse,
 * focus and close update the struct in place.  MXEV_NONE is ignored. */
static void mx_apply(mx_conn *c, const ipc_msg_t *m)
{
    switch (m->type) {
    case MXEV_KEY: {
        int nh = (c->kh + 1) % MX_KEYBUF;
        if (nh != c->kt) { c->keys[c->kh] = (int)m->data[0]; c->kh = nh; }
        break;
    }
    case MXEV_MOUSE:
        c->mx    = (int)m->data[0];
        c->my    = (int)m->data[1];
        c->mdown = (int)(m->data[2] & 1u);
        break;
    case MXEV_FOCUS:
        c->focused = (int)m->data[0];
        break;
    case MXEV_CLOSE:
        c->closed = 1;
        break;
    default: break;
    }
}

int mx_connect(mx_conn *c, int argc, char **argv, int w, int h)
{
    for (unsigned i = 0; i < sizeof *c; i++) ((unsigned char *)c)[i] = 0;
    c->server = -1; c->win = -1; c->sid = -1;

    for (int i = 1; i + 1 < argc; i++)
        if (mx_streq(argv[i], "-makx")) { c->server = mx_atoi(argv[i + 1]); break; }
    if (c->server <= 0) return -1;

    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_HELLO; m.data[0] = (unsigned)w; m.data[1] = (unsigned)h;
    if (sys_ipc_sendrec(c->server, &m) != 0) return -1;

    int win = (int)m.data[0], sid = (int)m.data[1];
    if (win < 0 || sid < 0) return -1;

    void *base = sys_surface_map(sid);
    if (!base) return -1;

    c->win = win; c->sid = sid;
    c->surf.px = (gfx_u32 *)base; c->surf.w = w; c->surf.h = h;
    c->kh = c->kt = 0;
    return 0;
}

int mx_pump(mx_conn *c)
{
    int prev = c->last_mdown;
    c->mpressed = c->mreleased = 0;
    if (c->closed) return -1;

    for (;;) {
        ipc_msg_t m;
        for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
        m.type = MX_POLL; m.data[0] = (unsigned)c->win;
        if (sys_ipc_sendrec(c->server, &m) != 0) { c->closed = 1; return -1; }
        if (m.type == MXEV_NONE) break;
        mx_apply(c, &m);
        if (m.data[MX_PENDING] == 0) break;
    }

    c->mpressed  =  c->mdown && !prev;
    c->mreleased = !c->mdown &&  prev;
    c->last_mdown = c->mdown;
    return 0;
}

int mx_key(mx_conn *c)
{
    if (c->kt == c->kh) return -1;
    int k = c->keys[c->kt];
    c->kt = (c->kt + 1) % MX_KEYBUF;
    return k;
}

void mx_present(mx_conn *c)
{
    if (c->closed) return;
    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_PRESENT; m.data[0] = (unsigned)c->win;
    if (sys_ipc_sendrec(c->server, &m) != 0) { c->closed = 1; return; }
    mx_apply(c, &m);     /* opportunistic event piggy-backed on the present ack */
}

void mx_close(mx_conn *c)
{
    if (c->server > 0 && c->win >= 0) {
        ipc_msg_t m;
        for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
        m.type = MX_BYE; m.data[0] = (unsigned)c->win;
        sys_ipc_sendrec(c->server, &m);   /* best effort; surface freed on exit */
    }
}
