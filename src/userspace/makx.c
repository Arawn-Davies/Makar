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
        c->rdown = (int)((m->data[2] & 2u) ? 1 : 0);
        break;
    case MXEV_FOCUS:
        c->focused = (int)m->data[0];
        break;
    case MXEV_CLOSE:
        c->closed = 1;
        break;
    case MXEV_RESIZE:
        /* Server asked us to re-flow to a new client size; applied after the
         * pump drain (a resizable client only -- see mx_pump). */
        c->pending_rw = (int)m->data[0];
        c->pending_rh = (int)m->data[1];
        break;
    default: break;
    }
}

int mx_connect(mx_conn *c, int argc, char **argv, int w, int h, int flags)
{
    for (unsigned i = 0; i < sizeof *c; i++) ((unsigned char *)c)[i] = 0;
    c->server = -1; c->win = -1; c->sid = -1; c->flags = flags;

    for (int i = 1; i + 1 < argc; i++)
        if (mx_streq(argv[i], "-makx")) { c->server = mx_atoi(argv[i + 1]); break; }
    /* No `-makx` handle (e.g. launched from a GUI terminal, not by the server):
     * discover the running display server the X11 $DISPLAY way.  Returns 0 when
     * there's no GUI session, so a true text-console app still falls through to
     * its fullscreen path. */
    if (c->server <= 0) c->server = sys_makx_server();
    if (c->server <= 0) return -1;

    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_HELLO; m.data[0] = (unsigned)w; m.data[1] = (unsigned)h;
    m.data[2] = (unsigned)flags;
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

int mx_open_window(mx_conn *dlg, const mx_conn *parent, int w, int h, int flags)
{
    for (unsigned i = 0; i < sizeof *dlg; i++) ((unsigned char *)dlg)[i] = 0;
    dlg->server = (parent ? parent->server : -1);
    dlg->win = -1; dlg->sid = -1;
    flags |= MX_F_DIALOG;
    dlg->flags = flags;
    if (dlg->server <= 0) return -1;

    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_HELLO; m.data[0] = (unsigned)w; m.data[1] = (unsigned)h; m.data[2] = (unsigned)flags;
    if (sys_ipc_sendrec(dlg->server, &m) != 0) return -1;

    int win = (int)m.data[0], sid = (int)m.data[1];
    if (win < 0 || sid < 0) return -1;
    void *base = sys_surface_map(sid);
    if (!base) return -1;

    dlg->win = win; dlg->sid = sid;
    dlg->surf.px = (gfx_u32 *)base; dlg->surf.w = w; dlg->surf.h = h;
    dlg->kh = dlg->kt = 0;
    return 0;
}

/* Re-flow to a new client size the server requested: ask it to reallocate our
 * surface (MX_RESIZE), map the new one and release the old.  Only for resizable
 * clients; on any failure we keep the current surface. */
static void mx_apply_resize(mx_conn *c, int w, int h)
{
    if (w <= 0 || h <= 0 || (w == c->surf.w && h == c->surf.h)) return;
    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_RESIZE; m.data[0] = (unsigned)c->win;
    m.data[1] = (unsigned)w; m.data[2] = (unsigned)h;
    if (sys_ipc_sendrec(c->server, &m) != 0) { c->closed = 1; return; }
    int nsid = (int)m.data[0];
    if (nsid < 0) return;                 /* server kept the old surface */
    void *base = sys_surface_map(nsid);
    if (!base) { c->closed = 1; return; } /* can't map the new one -> broken */
    int old = c->sid;
    c->sid = nsid; c->surf.px = (gfx_u32 *)base; c->surf.w = w; c->surf.h = h;
    c->resized = 1;
    sys_surface_unmap(old);               /* free the old frames + map slot */
}

int mx_pump(mx_conn *c)
{
    int cur = c->last_mdown;       /* running button state across this drain */
    int curR = c->last_rdown;      /* same for the right button              */
    c->mpressed = c->mreleased = 0;
    c->rpressed = c->rreleased = 0;
    c->resized = 0;
    if (c->closed) return -1;

    for (;;) {
        ipc_msg_t m;
        for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
        m.type = MX_POLL; m.data[0] = (unsigned)c->win;
        if (sys_ipc_sendrec(c->server, &m) != 0) { c->closed = 1; return -1; }
        if (m.type == MXEV_NONE) break;
        /* Latch press/release *per event* so a full click (down then up) that
         * arrives within a single pump isn't collapsed away -- looking only at
         * the final mdown vs the last pump dropped such clicks (the "needs a
         * double-click" lag). */
        if (m.type == MXEV_MOUSE) {
            int nd = (int)(m.data[2] & 1u);
            if (nd && !cur) c->mpressed = 1;
            if (!nd && cur) c->mreleased = 1;
            cur = nd;
            int nr = (int)((m.data[2] & 2u) ? 1 : 0);
            if (nr && !curR) c->rpressed = 1;
            if (!nr && curR) c->rreleased = 1;
            curR = nr;
        }
        mx_apply(c, &m);
        if (m.data[MX_PENDING] == 0) break;
    }

    /* Apply a pending resize once the event queue is drained (resizable only). */
    if (c->pending_rw && (c->flags & MX_F_RESIZABLE)) {
        int w = c->pending_rw, h = c->pending_rh;
        c->pending_rw = c->pending_rh = 0;
        mx_apply_resize(c, w, h);
    }

    c->last_mdown = c->mdown;       /* = cur (the final applied state) */
    c->last_rdown = c->rdown;
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

void mx_set_wallpaper(mx_conn *c, int sid, int w, int h)
{
    if (c->closed || c->server <= 0) return;
    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_WALLPAPER;
    m.data[0] = (unsigned)sid; m.data[1] = (unsigned)w; m.data[2] = (unsigned)h;
    sys_ipc_sendrec(c->server, &m);   /* server maps the surface + repaints */
}

void mx_reload_prefs(mx_conn *c)
{
    if (c->closed || c->server <= 0) return;
    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_RELOAD_PREFS;
    sys_ipc_sendrec(c->server, &m);
}

int mx_open(mx_conn *c, const char *path)
{
    if (c->closed || c->server <= 0 || !path || !path[0]) return -1;
    int len = 0; while (path[len] && len < 255) len++;
    /* Carry the path bytes in a throwaway 1-row surface (4 bytes/px).  The server
     * maps it, copies the path out and launches; we tear it down once it replies
     * (a synchronous sendrec, so the read has happened by then). */
    int px = (len + 4) / 4;            /* room for the path + a NUL, in pixels */
    int sid = sys_surface_create(px, 1);
    if (sid < 0) return -1;
    unsigned char *base = (unsigned char *)sys_surface_map(sid);
    if (!base) { sys_surface_destroy(sid); return -1; }
    for (int i = 0; i < len; i++) base[i] = (unsigned char)path[i];
    base[len] = 0;
    ipc_msg_t m;
    for (unsigned i = 0; i < IPC_MSG_DATA_WORDS; i++) m.data[i] = 0;
    m.type = MX_OPEN; m.data[0] = (unsigned)sid; m.data[1] = (unsigned)len;
    int rc = sys_ipc_sendrec(c->server, &m);
    sys_surface_unmap(sid); sys_surface_destroy(sid);
    return rc;
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
