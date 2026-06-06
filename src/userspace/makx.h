/*
 * makx.h -- the Makar display-server protocol + client library (an X11-ish
 * split for the GUI).  gui.elf is the *display server*: it owns the real
 * framebuffer, the keyboard and the mouse, draws the desktop chrome (window
 * borders, dock, menu bar) and composites client windows.  Every application
 * is a separate process -- a *client* -- that talks to the server over the
 * kernel's synchronous IPC (kernel/ipc.h) for control and a shared pixel
 * surface (kernel/surface.h) for pixels.  Control over the message channel,
 * bulk pixels over shared memory: the same division X11 draws between the
 * protocol socket and MIT-SHM.
 *
 * Discovery: the server launches each client with `-makx <server-pid>` in
 * argv, so mx_connect() finds the server without any name service.
 *
 * Flow (one window per client, the common case):
 *     mx_conn c;
 *     if (mx_connect(&c, argc, argv, W, H) != 0) return 1;   // HELLO + map
 *     while (!c.closed) {
 *         mx_pump(&c);                 // drain input events into c
 *         int k; while ((k = mx_key(&c)) >= 0) handle_key(k);
 *         ... draw into c.surf (a gfx_surface) ...
 *         mx_present(&c);              // flush the frame to the server
 *         sys_yield();
 *     }
 *     mx_close(&c);
 *
 * The pixels the client draws into c.surf live in the shared surface; the
 * server blits/scales that surface into the window's client rect each frame.
 * The client never calls sys_fb_present -- only the server (the focused
 * root-GUI task) may, so the kernel focus model is untouched.
 */
#ifndef MAKX_H
#define MAKX_H

#include "gui_gfx.h"

/* ---- wire protocol (ipc_msg_t.type) ------------------------------------- */
/* client -> server requests (sent with sys_ipc_sendrec) */
#define MX_HELLO    1   /* data[0]=w data[1]=h     -> reply data[0]=win data[1]=sid */
#define MX_PRESENT  2   /* data[0]=win             -> reply = one event (below)     */
#define MX_POLL     3   /* data[0]=win             -> reply = one event (below)     */
#define MX_BYE      4   /* data[0]=win             -> reply: ack                    */

/* server -> client reply event kinds (carried in the reply ipc_msg_t.type).
 * data[5] always carries the count of further events still queued, so the
 * client can drain with MX_POLL until it reads 0. */
#define MXEV_NONE   0   /* nothing queued                                    */
#define MXEV_KEY    1   /* data[0]=key (ASCII or KEY_* sentinel)             */
#define MXEV_MOUSE  2   /* data[0]=x data[1]=y (client-relative) data[2]=btn */
#define MXEV_FOCUS  3   /* data[0]=1 gained focus / 0 lost                   */
#define MXEV_CLOSE  4   /* the window should close (title-bar X, or quit)    */

#define MX_PENDING  5   /* data[] index carrying the queued-event count      */

/* ---- client connection state ------------------------------------------- */
#define MX_KEYBUF 64
typedef struct {
    int          server;        /* server pid (from -makx)                   */
    int          win;           /* our window id on the server               */
    int          sid;           /* shared surface id                         */
    gfx_surface  surf;          /* mapped pixels (draw here)                 */

    /* input snapshot, refreshed by mx_pump() -- feed straight into ui_begin */
    int          mx, my;        /* cursor, client-relative pixels            */
    int          mdown;         /* left button currently held                */
    int          mpressed;      /* left button edge: went down this pump     */
    int          mreleased;     /* left button edge: went up this pump       */
    int          focused;       /* server reports we have the keyboard       */
    int          closed;        /* server asked us to close (or it died)     */

    int          last_mdown;    /* edge tracking across pumps                */
    int          keys[MX_KEYBUF];
    int          kh, kt;        /* key ring head/tail                        */
} mx_conn;

/* Connect to the server named by `-makx <pid>` in argv and create one window
 * of `w`x`h` pixels.  Maps the server-allocated surface into c->surf.  Returns
 * 0 on success, -1 on failure (no -makx arg, server gone, surface map failed). */
int  mx_connect(mx_conn *c, int argc, char **argv, int w, int h);

/* Drain all queued input events from the server into c (mouse/focus/close are
 * folded into the struct; keys are buffered -- read them with mx_key()).
 * Computes mpressed/mreleased edges for this pump.  Returns 0, or -1 if the
 * server has gone away (c->closed is also set). */
int  mx_pump(mx_conn *c);

/* Pop the next buffered key (>=0), or -1 when the buffer is empty. */
int  mx_key(mx_conn *c);

/* Flush the current contents of c->surf to the server for compositing.  Also
 * folds back one opportunistically-returned event.  Call once per frame you
 * actually redrew. */
void mx_present(mx_conn *c);

/* Tell the server we're done and unmap.  Safe to call after the server died. */
void mx_close(mx_conn *c);

#endif /* MAKX_H */
