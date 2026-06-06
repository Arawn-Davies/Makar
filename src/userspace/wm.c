/*
 * wm.c -- gui.elf: the Makar display server ("makx").
 *
 * This process owns the framebuffer (it is the single focused root-GUI task,
 * the only one the kernel lets call SYS_FB_PRESENT), the keyboard and the
 * mouse.  It draws the desktop chrome -- window borders, the dock, the top menu
 * bar, the cursor -- and *composites* application windows.  Applications are no
 * longer built in: each one is a separate client process that talks to this
 * server over the kernel's synchronous IPC for control (makx.h) and a shared
 * pixel surface for pixels (kernel/surface.h).  Control over the message
 * channel, bulk pixels over shared memory -- the division X11 draws between its
 * protocol socket and MIT-SHM.  See docs/gui.md.
 *
 * The server is purely *reactive* to client requests + hardware input: it never
 * sends a client an unsolicited message.  Each client HELLO/PRESENT/POLL is a
 * sendrec; the reply carries at most one queued input event for that window
 * plus a "still pending" count, so a client drains its events by polling until
 * the count is zero.  The server interleaves draining client requests
 * (non-blocking sys_ipc_nbrecv) with polling the keyboard/mouse and compositing
 * -- it never parks in a blocking receive.
 *
 * Still built into the server (deliberately, for this cut): the desktop, dock,
 * menu bar and window chrome (a compositor-owned panel + WM, like many simple
 * stacks), and the graphical login screen (it is tied to the session/auth
 * handshake).  Migrating the panel + login out to their own clients is the
 * remaining step noted in docs/gui.md.
 *
 * Assumes a 32-bpp XRGB8888 framebuffer (QEMU Bochs VBE default).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "makx.h"

/* ---- framebuffer / back buffer ----------------------------------------- */

static unsigned int  FBW, FBH;
static gfx_surface   scr;              /* the server back buffer (scr.px==bb) */

#define RGB GFX_RGB
#define COL_DESK    RGB(0x1e,0x29,0x3b)
#define COL_WIN     RGB(0x16,0x1b,0x24)
#define COL_TITLE   RGB(0x35,0x6a,0xa8)
#define COL_TITLE_U RGB(0x24,0x48,0x74)   /* unfocused title bar             */
#define COL_TITLE2  RGB(0x24,0x48,0x74)
#define COL_BORDER  RGB(0x07,0x09,0x0d)
#define COL_CLOSE   RGB(0xc0,0x40,0x40)
#define COL_TEXT    RGB(0xd3,0xd7,0xcf)

#define TH       20          /* title-bar height        */
#define DOCK_H   34
#define MENU_H   22          /* top menu bar height     */
#define COL_MENU RGB(0x0c,0x10,0x18)

/* ---- small libc ---------------------------------------------------------- */
static int   slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static void  scpy(char *d, const char *s, int max){ int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static char *u2s(unsigned int v, char *out){ char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10u);v/=10u;} int j=0; while(i)out[j++]=t[--i]; out[j]=0; return out+j; }
static int   seq(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return a[i]==b[i]; }

/* ===================== window table (client-backed) ===================== */

#define MAXWIN 8
#define EVQ    32
typedef struct { int type, d0, d1, d2; } mxev;
typedef struct {
    int          in_use;
    int          client;        /* client pid                              */
    int          out;           /* client stdout/stderr pipe we drain (-1) */
    int          icon;          /* launching icon index (-1 if none)       */
    int          sid;           /* shared surface id (-1 until HELLO)       */
    gfx_surface  surf;          /* our mapping of the client surface        */
    int          x, y, w, h;    /* outer rect                              */
    int          minimized, maximized;
    int          resizable;     /* MX_F_RESIZABLE: re-flow (blit 1:1) vs scale */
    int          sx, sy, sw, sh;/* geometry saved before maximise          */
    char         title[40];
    mxev         ev[EVQ]; int eh, et;     /* per-window event queue        */
} swin;

static swin W[MAXWIN];
static int  zorder[MAXWIN];     /* back(0) -> front, holds window indices  */
static int  znum;
static int  focus = -1;
static int  server_pid;
static int  g_dirty = 1;          /* back buffer needs a recompose this frame   */

/* Damage rectangle: the screen region that actually changed, so we present only
 * that (never the whole framebuffer) -- what a real compositor does.  Recompose
 * is cheap cacheable RAM; the framebuffer push is the cost (especially on VT-x
 * hypervisors where it's write-combined MMIO), so scoping it to the damage rect
 * makes a window repaint independent of how many other windows are open. */
static int  dmg_v=0, dmg_x0,dmg_y0,dmg_x1,dmg_y1;
static void damage(int x,int y,int w,int h){
    int x1=x+w, y1=y+h;
    if(!dmg_v){ dmg_x0=x; dmg_y0=y; dmg_x1=x1; dmg_y1=y1; dmg_v=1; }
    else { if(x<dmg_x0)dmg_x0=x; if(y<dmg_y0)dmg_y0=y; if(x1>dmg_x1)dmg_x1=x1; if(y1>dmg_y1)dmg_y1=y1; }
}
/* Damage a window including its 3px decoration border. */
static void damage_win(int i){ damage(W[i].x-4, W[i].y-4, W[i].w+8, W[i].h+8); }
static void damage_full(void){ damage(0,0,(int)FBW,(int)FBH); }

static int  client_x(swin *w){ return w->x + 1; }
static int  client_y(swin *w){ return w->y + TH; }
static int  client_w(swin *w){ return w->w - 2; }
static int  client_h(swin *w){ return w->h - TH - 1; }

/* z-order ------------------------------------------------------------------ */
static void z_remove(int i){ int j=0; while(j<znum && zorder[j]!=i) j++; if(j>=znum) return; for(;j<znum-1;j++) zorder[j]=zorder[j+1]; znum--; }
static void z_raise(int i){ z_remove(i); zorder[znum++]=i; }
static int  z_topmost(void){ for(int j=znum-1;j>=0;j--){ int i=zorder[j]; if(W[i].in_use && !W[i].minimized) return i; } return -1; }

/* per-window event queue --------------------------------------------------- */
static int  evq_pending(swin *s){ return (s->eh - s->et + EVQ) % EVQ; }
static void win_push(swin *s, int type, int d0, int d1, int d2)
{
    /* coalesce a run of mouse events that share the same button state -- keeps
     * pure cursor moves from flooding the queue while preserving transitions. */
    if (type==MXEV_MOUSE && s->eh!=s->et){
        int last=(s->eh-1+EVQ)%EVQ;
        if (s->ev[last].type==MXEV_MOUSE && s->ev[last].d2==d2){
            s->ev[last].d0=d0; s->ev[last].d1=d1; return;
        }
    }
    int nh=(s->eh+1)%EVQ;
    if (nh==s->et) s->et=(s->et+1)%EVQ;        /* full: drop oldest */
    s->ev[s->eh].type=type; s->ev[s->eh].d0=d0; s->ev[s->eh].d1=d1; s->ev[s->eh].d2=d2;
    s->eh=nh;
}
static void win_pop(swin *s, ipc_msg_t *r)
{
    for(int i=0;i<IPC_MSG_DATA_WORDS;i++) r->data[i]=0;
    if (s->et==s->eh){ r->type=MXEV_NONE; return; }
    mxev e=s->ev[s->et]; s->et=(s->et+1)%EVQ;
    r->type=e.type; r->data[0]=(unsigned)e.d0; r->data[1]=(unsigned)e.d1; r->data[2]=(unsigned)e.d2;
    r->data[MX_PENDING]=(unsigned)evq_pending(s);
}

static int win_alloc(void){ for(int i=0;i<MAXWIN;i++) if(!W[i].in_use){ for(unsigned b=0;b<sizeof W[i];b++)((unsigned char*)&W[i])[b]=0; W[i].in_use=1; W[i].client=-1; W[i].out=-1; W[i].icon=-1; W[i].sid=-1; return i; } return -1; }

static void set_focus(int i)
{
    if (focus==i) return;
    if (focus>=0 && W[focus].in_use) win_push(&W[focus], MXEV_FOCUS, 0,0,0);
    focus=i;
    if (i>=0 && W[i].in_use) win_push(&W[i], MXEV_FOCUS, 1,0,0);
    g_dirty=1; damage_full();   /* both borders recolour: simplest to repaint all */
}
static void refocus(void){ set_focus(z_topmost()); }

/* free a window slot whose client has gone (reaped) or been told to close */
static void win_free(int i)
{
    if (!W[i].in_use) return;
    if (W[i].sid>=0) sys_surface_destroy(W[i].sid);
    if (W[i].out>=0) sys_close(W[i].out);
    z_remove(i);
    W[i].in_use=0; W[i].client=-1; W[i].out=-1; W[i].sid=-1; W[i].surf.px=0;
    if (focus==i){ focus=-1; refocus(); }
    g_dirty=1; damage_full();   /* area behind the closed window must repaint */
}

/* ===================== desktop icons (client launchers) ================== */
/* Each icon names a client *.elf and the default outer window geometry.  The
 * program path is launcher data -- the server bakes in no application. */
typedef struct { int x,y,w,h; const char *label; gfx_u32 tint; const char *cmd; int winw, winh; } icon_t;
#define ICON_N 6
/* winw/winh are sized so each client's fixed surface (mxterm 640x400, mxfiles
 * 560x380, mxedit 620x420, mxtasks 560x360, doom 640x400, mxabout 560x430) fits
 * the window's client rect 1:1 (client_w = winw-2, client_h = winh-TH-1). */
static icon_t icons[ICON_N] = {
    { 24,  40, 96,70, "Terminal", RGB(0x4c,0x8d,0xff), "/apps/mxterm.elf",  648,424 },
    { 24, 124, 96,70, "Files",    RGB(0xf0,0xa8,0x30), "/apps/mxfiles.elf", 568,404 },
    { 24, 208, 96,70, "Editor",   RGB(0x35,0xc7,0x59), "/apps/mxedit.elf",  628,444 },
    { 24, 292, 96,70, "Tasks",    RGB(0x9b,0x6c,0xff), "/apps/mxtasks.elf", 568,384 },
    { 24, 376, 96,70, "Doom",     RGB(0xc0,0x40,0x40), "/apps/doom.elf",    648,424 },
    { 24, 460, 96,70, "About",    RGB(0x35,0x6a,0xa8), "/apps/mxabout.elf", 568,454 },
};

/* Fork+exec a client, handing it `-makx <server-pid>` and a stdout/stderr pipe
 * the server drains (so client console noise can't bleed onto the text VT and a
 * full pipe never blocks the client).  A window slot is reserved immediately;
 * the client's HELLO fills in the surface.  If the icon's window is already
 * open, just raise it. */
static void launch_icon(int ii)
{
    for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].icon==ii){ W[i].minimized=0; z_raise(i); set_focus(i); g_dirty=1; damage_full(); return; }
    int i=win_alloc(); if(i<0) return;
    int op[2];
    if (sys_pipe(op)<0){ W[i].in_use=0; return; }
    int pid=sys_fork();
    if (pid<0){ sys_close(op[0]); sys_close(op[1]); W[i].in_use=0; return; }
    if (pid==0){
        sys_close(op[0]);
        /* Redirect stdin off the inherited keyboard: on execve the kernel hands
         * keyboard focus to the new program if its fd 0 is the keyboard (the
         * "thing you just exec'd takes the keyboard" rule).  A makx client reads
         * input over IPC, not fd 0, so if it grabbed focus the real keys would
         * route to its slot and never be drained -- the server would go deaf.
         * We point fd 0 at the drain pipe (a pipe, not the keyboard -> no focus
         * grab) rather than CLOSING it: a closed fd 0 gets reused by the next
         * sys_pipe(), and a client that forks a child over pipes (mxterm) would
         * then close its child's real stdin by accident. */
        sys_dup2(op[1],0); sys_dup2(op[1],1); sys_dup2(op[1],2);
        sys_close(op[1]);
        char pids[12]; u2s((unsigned)server_pid, pids);
        char *av[4]={ (char*)icons[ii].cmd, "-makx", pids, 0 };
        sys_execve(icons[ii].cmd, av, (char *const*)0);
        sys_exit(127);
    }
    sys_close(op[1]);
    sys_fcntl(op[0], F_SETFL, O_NONBLOCK);
    W[i].client=pid; W[i].out=op[0]; W[i].icon=ii; W[i].sid=-1;
    scpy(W[i].title, icons[ii].label, sizeof W[i].title);
    W[i].w=icons[ii].winw; W[i].h=icons[ii].winh;
    W[i].x=120+(i*30)%220; W[i].y=MENU_H+24+(i*26)%150;
    z_raise(i); set_focus(i); g_dirty=1; damage_full();
}

/* Ask a resizable window's client to re-flow to the current client rect (it
 * reallocates its surface via MX_RESIZE).  No-op for fixed (scaled) clients or
 * when the surface already matches.  Sent on connect + every geometry change. */
static void maybe_send_resize(int i)
{
    if (i < 0 || !W[i].in_use || !W[i].resizable) return;
    int cw = client_w(&W[i]), ch = client_h(&W[i]);
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if (cw != W[i].sw || ch != W[i].sh)
        win_push(&W[i], MXEV_RESIZE, cw, ch, 0);
}

/* ===================== client request servicing ========================== */
static int win_valid(int i, int src){ return i>=0 && i<MAXWIN && W[i].in_use && W[i].client==src; }

static void serve_requests(void)
{
    /* Drain at most a bounded number of client requests per server frame, then
     * return so the loop composites and yields.  Clients busy-poll (a POLL
     * sendrec every frame), so an unbounded "while there's a message" drain
     * would spin forever on a couple of live clients and starve everything else
     * -- including not-yet-connected clients waiting to send their first HELLO.
     * The cap guarantees forward progress for all tasks; deferred requests are
     * simply served on the next frame (clients block harmlessly until then). */
    int budget = 4 * MAXWIN;
    ipc_msg_t m;
    while (budget-- > 0 && sys_ipc_nbrecv(IPC_ANY, &m) == 0){
        int src=m.src;
        ipc_msg_t r; for(int i=0;i<IPC_MSG_DATA_WORDS;i++) r.data[i]=0; r.type=MXEV_NONE;

        if (m.type==MX_HELLO){
            int w=(int)m.data[0], h=(int)m.data[1], flags=(int)m.data[2];
            int i=-1;
            for(int k=0;k<MAXWIN;k++) if(W[k].in_use && W[k].client==src && W[k].sid<0){ i=k; break; }
            if (i<0){ /* a client we didn't reserve: give it a default window */
                i=win_alloc();
                if (i>=0){ W[i].client=src; W[i].w=w<160?160:w; W[i].h=h<120?120:h;
                           W[i].x=140; W[i].y=MENU_H+40; scpy(W[i].title,"App",sizeof W[i].title); }
            }
            int sid = (i>=0) ? sys_surface_create(w,h) : -1;
            void *base = (sid>=0) ? sys_surface_map(sid) : 0;
            if (i<0 || sid<0 || !base){
                if (sid>=0) sys_surface_destroy(sid);
                r.data[0]=(unsigned)-1; r.data[1]=(unsigned)-1;
            } else {
                W[i].sid=sid; W[i].surf.px=(gfx_u32*)base; W[i].surf.w=w; W[i].surf.h=h; W[i].sw=w; W[i].sh=h;
                W[i].resizable = (flags & MX_F_RESIZABLE) ? 1 : 0;
                r.data[0]=(unsigned)i; r.data[1]=(unsigned)sid;
                z_raise(i); set_focus(i); g_dirty=1; damage_full();
                /* A resizable client re-flows to fill: ask it to size its surface
                 * to the actual client rect (the window geometry, not the
                 * initial request) so the blit is exact 1:1 from the first frame. */
                maybe_send_resize(i);
            }
        } else if (m.type==MX_RESIZE){
            int i=(int)m.data[0], w=(int)m.data[1], h=(int)m.data[2];
            if (win_valid(i,src) && w>0 && h>0){
                int ns = sys_surface_create(w,h);
                void *nb = (ns>=0) ? sys_surface_map(ns) : 0;
                if (ns<0 || !nb){            /* keep the old surface on failure */
                    if (ns>=0) sys_surface_destroy(ns);
                    r.data[0]=(unsigned)-1;
                } else {
                    int old = W[i].sid;
                    sys_surface_unmap(old);    /* drop the server's mapping of the old one */
                    sys_surface_destroy(old);  /* drop creator ref (client still maps it until it unmaps) */
                    W[i].sid=ns; W[i].surf.px=(gfx_u32*)nb; W[i].surf.w=w; W[i].surf.h=h; W[i].sw=w; W[i].sh=h;
                    r.data[0]=(unsigned)ns; g_dirty=1; damage_full();
                }
            } else r.data[0]=(unsigned)-1;
        } else if (m.type==MX_PRESENT){
            int i=(int)m.data[0];
            if (win_valid(i,src)){ g_dirty=1; damage_win(i); win_pop(&W[i],&r); }
            else r.type=MXEV_CLOSE;
        } else if (m.type==MX_POLL){
            int i=(int)m.data[0];
            if (win_valid(i,src)) win_pop(&W[i],&r);
            else r.type=MXEV_CLOSE;
        } else if (m.type==MX_BYE){
            /* The client acks then exits; the reap loop frees the slot. */
            r.type=MXEV_NONE;
        }
        sys_ipc_send(src, &r);
    }
}

/* Reap exited client children; drain their stdout so it never bleeds/blocks. */
static int reap_clients(void)
{
    int changed=0;
    for(int i=0;i<MAXWIN;i++){
        if(!W[i].in_use || W[i].client<=0) continue;
        if(W[i].out>=0){ unsigned char b[128]; while(sys_read(W[i].out,b,sizeof b)>0){} }
        int st; if(sys_wait4(W[i].client,&st,WNOHANG)==W[i].client){ W[i].client=-1; win_free(i); changed=1; }
    }
    return changed;
}

/* ===================== window chrome + compositor ======================= */
#define BTN_D   11
#define BTN_GAP 7
static int btn_close_x(swin *w){ return w->x + w->w - 8 - BTN_D; }
static int btn_max_x  (swin *w){ return btn_close_x(w) - (BTN_D + BTN_GAP); }
static int btn_min_x  (swin *w){ return btn_max_x(w)   - (BTN_D + BTN_GAP); }
static int btn_y      (swin *w){ return w->y + (TH - BTN_D) / 2; }

static void win_toggle_max(int i)
{
    swin *w=&W[i];
    if (!w->maximized){
        w->sx=w->x; w->sy=w->y; w->sw=w->w; w->sh=w->h;
        w->x=0; w->y=MENU_H; w->w=(int)FBW; w->h=(int)FBH-MENU_H-DOCK_H;
        w->maximized=1;
    } else {
        w->x=w->sx; w->y=w->sy; w->w=w->sw; w->h=w->sh;
        w->maximized=0;
    }
    maybe_send_resize(i);   /* re-flow the client to the new client rect */
    g_dirty=1; damage_full();
}

static void draw_window_frame(int i)
{
    swin *w=&W[i];
    int focused=(focus==i);
    gfx_u32 border = focused ? UI_COL_BTN_ACT : RGB(0x3a,0x4e,0x6e);
    gfx_outline(&scr, w->x-3, w->y-3, w->w+6, w->h+6, COL_BORDER);
    gfx_outline(&scr, w->x-2, w->y-2, w->w+4, w->h+4, border);
    gfx_outline(&scr, w->x-1, w->y-1, w->w+2, w->h+2, border);
    gfx_fill(&scr, w->x, w->y, w->w, w->h, COL_WIN);
    gfx_fill(&scr, w->x, w->y, w->w, TH, focused?COL_TITLE:COL_TITLE_U);
    gfx_fill(&scr, w->x, w->y+TH-2, w->w, 2, COL_TITLE2);
    gfx_str_clip(&scr, w->x+10, w->y+(TH-8)/2, w->title, 0xFFFFFF, btn_min_x(w)-6);
    int by=btn_y(w);
    gfx_round(&scr, btn_min_x(w),   by, BTN_D, BTN_D, focused?RGB(0xfe,0xbc,0x2e):RGB(0x5e,0x57,0x40), COL_BORDER);
    gfx_round(&scr, btn_max_x(w),   by, BTN_D, BTN_D, focused?RGB(0x28,0xc8,0x40):RGB(0x46,0x5a,0x46), COL_BORDER);
    gfx_round(&scr, btn_close_x(w), by, BTN_D, BTN_D, focused?RGB(0xff,0x5f,0x57):RGB(0x6a,0x4a,0x48), COL_BORDER);
    {   /* resize grip: a solid corner wedge (bottom-right) */
        gfx_u32 grip = focused ? UI_COL_BTN_ACT : RGB(0x4a,0x5a,0x74);
        for (int r=0;r<14;r++) gfx_fill(&scr, w->x+w->w-1-r, w->y+w->h-1-r, r+1, 1, grip);
    }
    /* client area: blit the client's surface so it always FILLS the window --
     * 1:1 only when the surface exactly matches the client rect, otherwise
     * nearest-neighbour scale to fill (so a resized window doesn't leave the
     * content sitting at its original size in the corner). */
    int cx=client_x(w), cy=client_y(w), cw=client_w(w), ch=client_h(w);
    if (W[i].sid>=0 && W[i].surf.px){
        if (W[i].resizable){
            /* re-flow client: its surface tracks the window, so blit 1:1.  Clip
             * to the overlap for the brief frame between a window resize and the
             * client's MX_RESIZE re-alloc landing (content stays native size,
             * the window just reveals more/less -- normal resize behaviour). */
            int bw = cw < W[i].sw ? cw : W[i].sw;
            int bh = ch < W[i].sh ? ch : W[i].sh;
            gfx_blit(&scr, cx, cy, &W[i].surf, 0,0, bw, bh);
        } else if (cw>=W[i].sw && ch>=W[i].sh){
            /* fixed-size client (e.g. doom): 1:1 centred when it fits (no scaling
             * cost) -- only scale when the window is smaller than the surface. */
            gfx_blit(&scr, cx+(cw-W[i].sw)/2, cy+(ch-W[i].sh)/2, &W[i].surf, 0,0, W[i].sw, W[i].sh);
        } else {
            gfx_blit_scaled(&scr, cx, cy, cw, ch, &W[i].surf);
        }
    } else {
        gfx_fill(&scr, cx, cy, cw, ch, RGB(0x0e,0x12,0x18));
        gfx_str(&scr, cx+8, cy+8, "starting...", UI_COL_MUTED);
    }
}

static int in_rect(int px,int py,int x,int y,int w,int h){ return px>=x&&px<x+w&&py>=y&&py<y+h; }
static int in_btn(int bx,int by,int px,int py){ return in_rect(px,py,bx-2,by-2,BTN_D+4,BTN_D+4); }
static int in_min  (swin *w,int px,int py){ return in_btn(btn_min_x(w),  btn_y(w),px,py); }
static int in_max  (swin *w,int px,int py){ return in_btn(btn_max_x(w),  btn_y(w),px,py); }
static int in_close(swin *w,int px,int py){ return in_btn(btn_close_x(w),btn_y(w),px,py); }
static int in_titlebar(swin *w,int px,int py){ return in_rect(px,py,w->x,w->y,btn_min_x(w)-w->x,TH); }
static int in_resize(swin *w,int px,int py){ return in_rect(px,py,w->x+w->w-18,w->y+w->h-18,20,20); }
static int in_client(swin *w,int px,int py){ return in_rect(px,py,client_x(w),client_y(w),client_w(w),client_h(w)); }

static int hit_window(int px,int py)
{
    for (int j=znum-1;j>=0;j--){ int i=zorder[j];
        if(W[i].in_use && !W[i].minimized && in_rect(px,py,W[i].x-1,W[i].y-1,W[i].w+2,W[i].h+2)) return i; }
    return -1;
}

/* Draw a recognisable per-app glyph in a ~32x26 box at (gx,gy).  Pixel art via
 * primitives -- no bitmap pipeline -- matched to each launcher by index. */
static void icon_glyph(int idx, int gx, int gy)
{
    gfx_u32 bg = RGB(0x2a,0x38,0x50);
    switch (idx) {
    case 0: /* Terminal: dark screen + green prompt + cursor */
        gfx_round(&scr,gx,gy,32,26,RGB(0x10,0x16,0x20),bg);
        gfx_outline(&scr,gx,gy,32,26,RGB(0x4c,0x8d,0xff));
        gfx_str(&scr,gx+4,gy+5,">",RGB(0x8a,0xe2,0x34));
        gfx_fill(&scr,gx+14,gy+5,7,7,RGB(0x8a,0xe2,0x34));
        break;
    case 1: /* Files: folder with a tab */
        gfx_fill(&scr,gx+1,gy+2,13,6,RGB(0xc8,0x8a,0x20));
        gfx_round(&scr,gx,gy+6,32,20,RGB(0xf0,0xa8,0x30),bg);
        gfx_fill(&scr,gx,gy+9,32,2,RGB(0xc8,0x8a,0x20));
        break;
    case 2: /* Editor: page with text lines + folded corner */
        gfx_fill(&scr,gx+5,gy,22,26,RGB(0xe6,0xea,0xf0));
        gfx_fill(&scr,gx+21,gy,6,6,bg);             /* folded corner */
        for(int k=0;k<4;k++) gfx_fill(&scr,gx+8,gy+6+k*5,15,2,RGB(0x90,0xa0,0xb5));
        gfx_outline(&scr,gx+5,gy,22,26,RGB(0x35,0xc7,0x59));
        break;
    case 3: { /* Tasks: a little bar chart */
        gfx_round(&scr,gx,gy,32,26,RGB(0x1b,0x22,0x2e),bg);
        static const int w[3]={24,15,20};
        for(int k=0;k<3;k++) gfx_fill(&scr,gx+3,gy+4+k*7,w[k],5,RGB(0x9b,0x6c,0xff));
        break; }
    case 4: /* Doom: angry red face */
        gfx_round(&scr,gx,gy,32,26,RGB(0xc0,0x40,0x40),bg);
        gfx_fill(&scr,gx+7,gy+8,5,6,RGB(0x20,0x06,0x06));
        gfx_fill(&scr,gx+20,gy+8,5,6,RGB(0x20,0x06,0x06));
        gfx_fill(&scr,gx+10,gy+18,12,3,RGB(0x20,0x06,0x06));
        break;
    case 5: /* About: info 'i' on a disc */
        gfx_round(&scr,gx+2,gy,28,26,RGB(0x35,0x6a,0xa8),bg);
        gfx_fill(&scr,gx+15,gy+5,3,3,0xFFFFFF);
        gfx_fill(&scr,gx+15,gy+10,3,11,0xFFFFFF);
        break;
    default:
        gfx_round(&scr,gx,gy,32,26,RGB(0x4c,0x8d,0xff),bg);
        break;
    }
}
static void draw_icons(void)
{
    for(int i=0;i<ICON_N;i++){ icon_t *c=&icons[i];
        gfx_round(&scr,c->x,c->y,c->w,c->h,RGB(0x2a,0x38,0x50),COL_DESK);
        icon_glyph(i, c->x+c->w/2-16, c->y+9);
        gfx_str(&scr,c->x+(c->w-gfx_text_w(c->label))/2,c->y+c->h-16,c->label,0xFFFFFF);
    }
}
static int icon_hit(int px,int py){ for(int i=0;i<ICON_N;i++){icon_t*c=&icons[i]; if(in_rect(px,py,c->x,c->y,c->w,c->h)) return i;} return -1; }

/* ---- system stats for the right of the dock (CPU% + RAM%) --------------- */
static unsigned dock_meminfo_kb(const char *label)
{
    char buf[512]; int fd=sys_open("/proc/meminfo",O_RDONLY); if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof buf-1); sys_close(fd); if(r<=0) return 0; buf[r]=0;
    int ll=slen(label);
    for(long i=0;i<r;){
        int j=0; while(j<ll && buf[i+j]==label[j]) j++;
        if(j==ll && buf[i+ll]==':'){ const char *p=buf+i+ll+1; while(*p==' ')p++;
            unsigned v=0; while(*p>='0'&&*p<='9'){v=v*10u+(unsigned)(*p-'0');p++;} return v; }
        while(i<r && buf[i]!='\n') i++; i++;
    }
    return 0;
}
static unsigned dock_busy_ticks(void)
{
    char buf[1024]; int fd=sys_open("/proc/tasks",O_RDONLY); if(fd<0) return 0;
    long r=sys_read(fd,buf,sizeof buf-1); sys_close(fd); if(r<=0) return 0; buf[r]=0;
    long i=0; int line=0; unsigned sum=0;
    while(i<r){
        char tok[6][20]; int nt=0;
        while(i<r && buf[i]!='\n'){
            while(i<r&&(buf[i]==' '||buf[i]=='\t'))i++;
            if(i>=r||buf[i]=='\n')break;
            int tl=0; while(i<r&&buf[i]!=' '&&buf[i]!='\t'&&buf[i]!='\n'){ if(nt<6&&tl<19)tok[nt][tl++]=buf[i]; i++; }
            if(nt<6){tok[nt][tl]=0;nt++;}
        }
        if(i<r)i++;
        if(line++==0)continue;
        if(nt<5)continue;
        if(tok[0][0]=='1'&&tok[0][1]==0)continue;     /* skip idle (pid 1) */
        unsigned v=0; for(int k=0;tok[4][k];k++)v=v*10u+(unsigned)(tok[4][k]-'0'); sum+=v;
    }
    return sum;
}
static void dock_stats(char *out)
{
    static unsigned last_busy=0,last_up=0,cpu=0,ram=0; static int have=0;
    unsigned up=sys_uptime();
    if(!have || (up-last_up)>=100u){
        unsigned busy=dock_busy_ticks();
        if(have && up>last_up){ unsigned dt=up-last_up, db=(busy>last_busy)?busy-last_busy:0u;
                                cpu=db*100u/dt; if(cpu>100u)cpu=100u; }
        unsigned tot=dock_meminfo_kb("MemTotal"), fr=dock_meminfo_kb("MemFree");
        ram=(tot>fr)?(tot-fr)*100u/tot:0u;
        last_busy=busy; last_up=up; have=1;
    }
    char n[8]; int o=0; const char *p;
    p="CPU "; while(*p)out[o++]=*p++; u2s(cpu,n); for(int i=0;n[i];i++)out[o++]=n[i]; out[o++]='%';
    out[o++]=' '; out[o++]=' ';
    p="RAM "; while(*p)out[o++]=*p++; u2s(ram,n); for(int i=0;n[i];i++)out[o++]=n[i]; out[o++]='%';
    out[o]=0;
}

/* dock: one tile per open window (kind/icon order for stable positions) */
static int dock_order[MAXWIN], dock_n;
static void dock_rebuild(void)
{
    dock_n=0;
    for(int ii=0;ii<ICON_N;ii++) for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].icon==ii) dock_order[dock_n++]=i;
    for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].icon<0) dock_order[dock_n++]=i;
}
static int dock_btn_x(int slot){ return 8 + slot*60; }
static void dock_short(int i, char *out)
{
    /* first 3 chars of the title, lower-ish -- a compact tab label */
    int j=0; for(; j<3 && W[i].title[j]; j++) out[j]=W[i].title[j]; out[j]=0;
}
static void draw_dock(void)
{
    int y0=(int)FBH-DOCK_H;
    gfx_fill(&scr,0,y0,(int)FBW,DOCK_H,RGB(0x12,0x16,0x1e));
    gfx_fill(&scr,0,y0,(int)FBW,1,RGB(0x28,0x32,0x44));
    dock_rebuild();
    for(int s=0;s<dock_n;s++){ int i=dock_order[s]; int bx=dock_btn_x(s); int active=(focus==i);
        gfx_round(&scr,bx,y0+5,54,DOCK_H-10, active?UI_COL_BTN_ACT:UI_COL_BTN, RGB(0x12,0x16,0x1e));
        char lbl[8]; dock_short(i,lbl);
        gfx_str(&scr,bx+(54-gfx_text_w(lbl))/2, y0+(DOCK_H-8)/2, lbl, 0xFFFFFF);
    }
    char st[32]; dock_stats(st);
    gfx_str(&scr,(int)FBW-gfx_text_w(st)-10, y0+(DOCK_H-8)/2, st, RGB(0x90,0xa0,0xb5));
}
static int dock_hit(int px,int py,int *out_win)
{
    int y0=(int)FBH-DOCK_H; if(py<y0) return 0;
    for(int s=0;s<dock_n;s++){ int bx=dock_btn_x(s); if(in_rect(px,py,bx,y0+5,54,DOCK_H-10)){ *out_win=dock_order[s]; return 1; } }
    return 0;
}

/* top menu bar -------------------------------------------------------------- */
#define LOGOFF_W 70
#define EXIT_W   54
static void draw_menubar(void)
{
    gfx_fill(&scr,0,0,(int)FBW,MENU_H,COL_MENU);
    gfx_fill(&scr,0,MENU_H-1,(int)FBW,1,RGB(0x28,0x32,0x44));
    gfx_str(&scr,8,(MENU_H-8)/2,"Makar",RGB(0x8a,0xe2,0x34));
    gfx_str(&scr,64,(MENU_H-8)/2, (focus>=0&&W[focus].in_use)?W[focus].title:"Desktop", RGB(0x90,0xa0,0xb5));
    int lx=(int)FBW-LOGOFF_W-4;
    gfx_fill(&scr,lx,2,LOGOFF_W,MENU_H-4,COL_CLOSE);
    gfx_str(&scr,lx+(LOGOFF_W-gfx_text_w("Log Off"))/2,(MENU_H-8)/2,"Log Off",0xFFFFFF);
    int ex=lx-EXIT_W-4;
    gfx_fill(&scr,ex,2,EXIT_W,MENU_H-4,UI_COL_BTN);
    gfx_str(&scr,ex+(EXIT_W-gfx_text_w("Exit"))/2,(MENU_H-8)/2,"Exit",0xFFFFFF);
}
static int exit_hit(int px,int py){ int ex=(int)FBW-LOGOFF_W-4-EXIT_W-4; return in_rect(px,py,ex,2,EXIT_W,MENU_H-4); }
static int logoff_hit(int px,int py){ int lx=(int)FBW-LOGOFF_W-4; return in_rect(px,py,lx,2,LOGOFF_W,MENU_H-4); }

/* ---- mouse cursor ------------------------------------------------------- */
static const char *CURSOR[16]={
 "X          ","XX         ","X.X        ","X..X       ","X...X      ","X....X     ",
 "X.....X    ","X......X   ","X.......X  ","X........X ","X....XXXXXX","X..X.X     ",
 "X.X X.X    ","XX  X.X    ","X    X.X   ","      XX   " };
static void draw_cursor(int cx,int cy)
{
    for(int r=0;r<16;r++) for(int c=0;CURSOR[r][c];c++){ char p=CURSOR[r][c];
        if(p=='X')gfx_px(&scr,cx+c,cy+r,0); else if(p=='.')gfx_px(&scr,cx+c,cy+r,0xFFFFFF); }
}

/* Cursor save-under: so the cursor can be moved without recompositing the whole
 * scene (a full recompose blits every window surface, so its cost grows with the
 * window count -- death by a thousand mouse moves).  We stash the scene pixels
 * the cursor covers, then restore them before redrawing it elsewhere, and push
 * only the small old/new boxes to the framebuffer via sys_fb_present_rect. */
#define CURW 12
#define CURH 16
static gfx_u32 cur_save[CURW*CURH];
static int     cur_sx=-1, cur_sy=-1;   /* where cur_save was captured (-1 = none) */

static void cursor_capture(int x,int y){
    for(int r=0;r<CURH;r++) for(int c=0;c<CURW;c++){
        int px=x+c, py=y+r;
        cur_save[r*CURW+c] = (px>=0&&px<(int)FBW&&py>=0&&py<(int)FBH) ? scr.px[py*scr.w+px] : 0;
    }
    cur_sx=x; cur_sy=y;
}
static void cursor_restore(void){
    if(cur_sx<0) return;
    for(int r=0;r<CURH;r++) for(int c=0;c<CURW;c++){
        int px=cur_sx+c, py=cur_sy+r;
        if(px>=0&&px<(int)FBW&&py>=0&&py<(int)FBH) scr.px[py*scr.w+px]=cur_save[r*CURW+c];
    }
}
/* Push one clamped rect of the back buffer to the framebuffer. */
static void present_rect(int x,int y,int w,int h){
    if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;}
    if(x+w>(int)FBW)w=(int)FBW-x; if(y+h>(int)FBH)h=(int)FBH-y;
    if(w>0&&h>0) sys_fb_present_rect(scr.px,x,y,w,h);
}

/* ============================== self-tests ============================== */
/* `gui.elf uitest` / `gui.elf fstest` are headless and run by shell-smoke.sh;
 * they keep the GUI-UITEST / GUI-FSTEST regression markers (no framebuffer is
 * touched -- they return before sys_fb_info).  The widget + browser code they
 * exercise is the same gui_ui / gui_browser the clients use. */
static void emit(const char *m){ int n=0; while(m[n])n++; sys_write_serial(m,n); }
static int uitest(void)
{
    static gfx_u32 px[64*64];
    gfx_surface s={ px, 64, 64 };
    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    int val=0; char box[16]; box[0]=0; int ok=1;
    ui_begin(&u,10,10,1,1,0,-1); ui_button(&u,&s,0,0,40,20,"B");
    ui_begin(&u,10,10,0,0,1,-1); int clicked=ui_button(&u,&s,0,0,40,20,"B");
    if (!clicked) ok=0;
    ui_begin(&u,0,30,1,1,0,-1);  ui_slider(&u,&s,0,24,40,12,&val,0,100);
    ui_begin(&u,40,30,1,0,0,-1); ui_slider(&u,&s,0,24,40,12,&val,0,100);
    if (val<90) ok=0;
    ui_begin(&u,10,46,1,1,0,-1); ui_textbox(&u,&s,0,40,40,12,box,16);
    ui_begin(&u,10,46,0,0,1,'Z'); ui_textbox(&u,&s,0,40,40,12,box,16);
    if (box[0]!='Z') ok=0;
    emit(ok? "GUI-UITEST: PASS\n" : "GUI-UITEST: FAIL\n");
    return ok?0:1;
}
static int seq2(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return a[i]==b[i]; }
static int fstest(void)
{
    static browser b; int ok=1;
    scpy(b.cwd, "/", sizeof b.cwd); b.sel=b.scroll=0; b.loaded=0;
    br_load(&b);
    if (b.n<=0) ok=0;
    if (!seq2(b.cwd, "/")) ok=0;
    int di=-1; for(int i=0;i<b.n;i++) if(b.type[i]==DT_DIR){ di=i; break; }
    if (di>=0){
        b.sel=di; br_enter_sel(&b);
        if (seq2(b.cwd, "/")) ok=0;
        if (b.n<0) ok=0;
        br_up(&b);
        if (!seq2(b.cwd, "/")) ok=0;
    } else { ok=0; }
    scpy(b.cwd, "/", sizeof b.cwd); b.sel=b.scroll=0; b.loaded=0; br_load(&b);
    emit(ok? "GUI-FSTEST: PASS\n" : "GUI-FSTEST: FAIL\n");
    return ok?0:1;
}

/* ============================ GUI login ================================= */
static void do_login(const char *prefill_user)
{
    char user[64]={0}, pass[64]={0}, err[40]={0};
    if (prefill_user && prefill_user[0]) scpy(user, prefill_user, sizeof user);
    int cx=(int)FBW/2, cy=(int)FBH/2, prev_left=0, dirty=1;
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    u.focus = user[0] ? 2 : 1;

    for(;;){
        int mpressed=0,mreleased=0; unsigned int ev;
        while((ev=sys_mouse_read())!=0){
            cx+=(int)(signed char)((ev>>8)&0xFF); cy+=(int)(signed char)((ev>>16)&0xFF);
            if(cx<0)cx=0; if(cx>=(int)FBW)cx=(int)FBW-1;
            if(cy<0)cy=0; if(cy>=(int)FBH)cy=(int)FBH-1;
            int left=ev&1; if(left&&!prev_left)mpressed=1; if(!left&&prev_left)mreleased=1;
            prev_left=left; dirty=1;
        }
        int mdown=prev_left, key=-1;
        { unsigned char b; if(sys_read(0,&b,1)==1){ key=b; dirty=1; } }
        if(!dirty){ sys_yield(); continue; }

        gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
        int pw=340,ph=190,px=(int)FBW/2-pw/2,py=(int)FBH/2-ph/2;
        gfx_round(&scr,px,py,pw,ph,COL_WIN,COL_BORDER);
        gfx_fill(&scr,px,py,pw,26,COL_TITLE);
        gfx_str(&scr,px+(pw-gfx_text_w("Makar -- sign in"))/2,py+9,"Makar -- sign in",0xFFFFFF);
        gfx_str(&scr,px+24,py+54,"User:",COL_TEXT);
        gfx_str(&scr,px+24,py+92,"Pass:",COL_TEXT);

        ui_begin(&u,cx,cy,mdown,mpressed,mreleased,
                 (key=='\t'||key=='\n'||key=='\r')?-1:key);
        ui_textbox(&u,&scr,px+72,py+48,pw-96,22,user,(int)sizeof user);
        ui_password(&u,&scr,px+72,py+86,pw-96,22,pass,(int)sizeof pass);
        int login_c=ui_button(&u,&scr,px+pw/2-44,py+128,88,28,"Log in");
        if(err[0]) gfx_str(&scr,px+24,py+ph-22,err,COL_CLOSE);

        if(key=='\t') u.focus = (u.focus==1)?2:1;
        if(login_c || key=='\n' || key=='\r'){
            if(user[0] && sys_login(user,pass)==0) return;
            scpy(err,"Incorrect credentials",sizeof err);
            pass[0]=0; u.focus=2;
        }
        draw_cursor(cx,cy);
        sys_fb_present(scr.px);
        dirty=0; sys_yield();
    }
}

/* =============================== main =================================== */
int main(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc>1 && seq(argv[1],"uitest")) return uitest();
    if (argc>1 && seq(argv[1],"fstest")) return fstest();
    int want_login = (argc>1 && seq(argv[1],"login"));
    const char *login_user = (want_login && argc>2) ? argv[2] : 0;

    unsigned info=sys_fb_info();
    if(!info){ const char*e="gui: no pixel framebuffer (VGA-only)\n"; sys_write(2,e,36); return 1; }
    FBW=(info>>16)&0xFFFF; FBH=info&0xFFFF;
    gfx_u32 *bb=(gfx_u32*)sys_mmap(0,(unsigned long)FBW*FBH*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (bb==(gfx_u32*)MAP_FAILED || !bb){ const char*e="gui: back-buffer mmap failed\n"; sys_write(2,e,29); return 1; }
    scr.px=bb; scr.w=(int)FBW; scr.h=(int)FBH;

    server_pid=sys_getpid();
    sys_fcntl(0,F_SETFL,O_NONBLOCK);
    int saved_status=sys_statusbar_enabled();
    sys_statusbar_set(0);
    sys_signal(SIGINT,SIG_IGN);

    if (want_login) do_login(login_user);

    znum=0; focus=-1;
    launch_icon(0);             /* open a terminal client on the desktop */

    int cx=(int)FBW/2, cy=(int)FBH/2, prev_left=0;
    int dragging=0, resizing=0, drag_win=-1, drag_dx=0, drag_dy=0;
    int announced=0, exit_to_shell=0;
    unsigned stat_up=0;

    for(;;){
        /* ---- gather hardware input ---- */
        int mpressed=0, mreleased=0;
        unsigned int ev;
        while((ev=sys_mouse_read())!=0){
            cx += (int)(signed char)((ev>>8)&0xFF);
            cy += (int)(signed char)((ev>>16)&0xFF);
            if(cx<0)cx=0; if(cx>=(int)FBW)cx=(int)FBW-1;
            if(cy<0)cy=0; if(cy>=(int)FBH)cy=(int)FBH-1;
            int left=ev&1;
            if(left&&!prev_left) mpressed=1;
            if(!left&&prev_left) mreleased=1;
            prev_left=left;
            /* NB: a pure cursor move does NOT dirty the scene -- it's handled by
             * the cheap cursor-only path below.  Scene changes (clicks, drags,
             * client repaints, focus) set g_dirty in their own handlers. */
        }
        int mdown=prev_left;
        int frame_key=-1;
        /* A key only changes the screen via the focused client's repaint (which
         * damages its own window); the WM chrome doesn't render keys, so this
         * doesn't dirty the scene by itself. */
        { unsigned char b; if (sys_read(0,&b,1)==1){ frame_key=b; } }
        int cmoved = (cx!=cur_sx || cy!=cur_sy);  /* cursor moved this frame? */

        /* ---- window-management click handling ---- */
        if (mpressed){
            int dk;
            if (logoff_hit(cx,cy)) break;
            else if (exit_hit(cx,cy)){ exit_to_shell=1; break; }
            else if (dock_hit(cx,cy,&dk)){ W[dk].minimized=0; z_raise(dk); set_focus(dk); g_dirty=1; damage_full(); }
            else {
                int hk=hit_window(cx,cy);
                if (hk>=0){
                    z_raise(hk); set_focus(hk); g_dirty=1; damage_full();
                    if      (in_close(&W[hk],cx,cy)) win_push(&W[hk],MXEV_CLOSE,0,0,0);
                    else if (in_min(&W[hk],cx,cy)){ W[hk].minimized=1; focus=-1; refocus(); }
                    else if (in_max(&W[hk],cx,cy)) win_toggle_max(hk);
                    else if (in_resize(&W[hk],cx,cy)){ resizing=1; drag_win=hk; W[hk].maximized=0; }
                    else if (in_titlebar(&W[hk],cx,cy)){ dragging=1; drag_win=hk; drag_dx=cx-W[hk].x; drag_dy=cy-W[hk].y; W[hk].maximized=0; }
                } else {
                    int ii=icon_hit(cx,cy);
                    if (ii>=0) launch_icon(ii);
                }
            }
        }
        if (mreleased){
            /* On finishing a resize drag, re-flow the client to the new size. */
            if (resizing && drag_win>=0) maybe_send_resize(drag_win);
            dragging=0; resizing=0;
        }
        if (dragging && drag_win>=0 && W[drag_win].in_use){ swin *w=&W[drag_win];
            damage_win(drag_win);                /* old position (erase trail) */
            w->x=cx-drag_dx; w->y=cy-drag_dy;
            if(w->x<0)w->x=0; if(w->y<MENU_H)w->y=MENU_H;
            if(w->x+w->w>(int)FBW)w->x=(int)FBW-w->w;
            if(w->y+w->h>(int)FBH-DOCK_H)w->y=(int)FBH-DOCK_H-w->h;
            g_dirty=1; damage_win(drag_win);     /* new position */
        }
        if (resizing && drag_win>=0 && W[drag_win].in_use){ swin *w=&W[drag_win];
            damage_win(drag_win);                /* old size */
            w->w=cx-w->x; w->h=cy-w->y;
            if(w->w<220)w->w=220; if(w->h<120)w->h=120;
            if(w->x+w->w>(int)FBW)w->w=(int)FBW-w->x;
            if(w->y+w->h>(int)FBH-DOCK_H)w->h=(int)FBH-DOCK_H-w->y;
            g_dirty=1; damage_win(drag_win);     /* new size */
        }

        /* ---- forward input to the focused client over IPC ---- */
        if (focus>=0 && W[focus].in_use){
            swin *w=&W[focus];
            if (frame_key>=0) win_push(w, MXEV_KEY, frame_key,0,0);
            /* Only forward the pointer when it actually moved or a button
             * changed -- a stationary cursor shouldn't keep waking the client
             * (which would repaint and force a recompose every frame). */
            if (cmoved || mpressed || mreleased){
                if (in_client(w,cx,cy)){
                    int rx=cx-client_x(w), ry=cy-client_y(w);
                    win_push(w, MXEV_MOUSE, rx, ry, mdown?1:0);
                } else if (mreleased){
                    win_push(w, MXEV_MOUSE, cx-client_x(w), cy-client_y(w), 0);
                }
            }
        }

        /* ---- service clients + reap exited ones ---- */
        serve_requests();
        if (reap_clients()){ g_dirty=1; damage_full(); }
        { unsigned now=sys_uptime(); if (now-stat_up>=100u){ stat_up=now; g_dirty=1;
            damage(0,0,(int)FBW,MENU_H);                       /* menu-bar clock */
            damage(0,(int)FBH-DOCK_H,(int)FBW,DOCK_H); } }      /* dock stats     */

        if (!g_dirty && !cmoved){ sys_yield(); continue; }

        if (g_dirty){
            /* ---- recompose the WHOLE back buffer (cheap, cacheable RAM) ---- */
            gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
            gfx_str(&scr,8,MENU_H+6,"Makar desktop -- click an icon; drag a title bar; click a window to focus",RGB(0x90,0xa0,0xb5));
            draw_icons();
            for (int j=0;j<znum;j++){ int i=zorder[j]; if(!W[i].in_use || W[i].minimized) continue; draw_window_frame(i); }
            draw_dock();
            draw_menubar();
            int ox=cur_sx, oy=cur_sy;
            cursor_capture(cx,cy);          /* stash scene under the cursor */
            draw_cursor(cx,cy);
            /* ---- but PUSH only the damaged region to the framebuffer ---- */
            if (!dmg_v) damage_full();      /* safety net for any untracked change */
            present_rect(dmg_x0,dmg_y0,dmg_x1-dmg_x0,dmg_y1-dmg_y0);
            if (cmoved && ox>=0) present_rect(ox,oy,CURW,CURH);  /* erase old cursor */
            present_rect(cx,cy,CURW,CURH);                       /* draw new cursor  */
            if (!announced){ sys_write_serial("GUI: READY\n", 11); announced=1; }
            g_dirty=0; dmg_v=0;
        } else {
            /* ---- cursor-only: O(cursor) regardless of window count ---- */
            int ox=cur_sx, oy=cur_sy;
            cursor_restore();                /* repaint scene under old cursor */
            cursor_capture(cx,cy);           /* stash scene under new cursor   */
            draw_cursor(cx,cy);
            present_rect(ox,oy,CURW,CURH);   /* flush old + new boxes only     */
            present_rect(cx,cy,CURW,CURH);
        }
        sys_yield();
    }

    /* Reached on Log Off or Exit.  Tear down every client child, restore the
     * statusbar, and hand the display back:
     *   Exit  -> sys_gui_close(): return to this session's CLI shell.
     *   LogOff-> sys_logout(): end the session; the login loop re-shows login. */
    /* A client parked in an IPC sendrec to us cannot process SIGKILL while it is
     * blocked, so a blocking wait4 here would deadlock (server frozen, last frame
     * stuck on screen).  Instead unblock each client by replying MXEV_CLOSE to
     * its in-flight request: it exits its loop, kills its own children, BYEs out,
     * and we reap it with WNOHANG.  Keep servicing IPC + reaping in a bounded
     * spin; SIGKILL + non-blocking reap any straggler that ignored CLOSE (it is
     * no longer IPC-blocked once we stop replying, so SIGKILL can land). */
    for(int i=0;i<MAXWIN;i++) if(W[i].in_use) win_push(&W[i],MXEV_CLOSE,0,0,0);
    for(int spin=0; spin<4000; spin++){
        ipc_msg_t m; int budget=4*MAXWIN;
        while(budget-->0 && sys_ipc_nbrecv(IPC_ANY,&m)==0){
            ipc_msg_t r; for(int k=0;k<IPC_MSG_DATA_WORDS;k++) r.data[k]=0; r.type=MXEV_CLOSE;
            sys_ipc_send(m.src,&r);
        }
        int live=0;
        for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].client>0){
            int st; if(sys_wait4(W[i].client,&st,WNOHANG)==W[i].client) win_free(i); else live=1;
        }
        if(!live) break;
        sys_yield();
    }
    for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].client>0){
        sys_kill(W[i].client,SIGKILL); int st; sys_wait4(W[i].client,&st,WNOHANG);
        win_free(i);
    }
    sys_fcntl(0,F_SETFL,0);
    sys_statusbar_set(saved_status);
    if (exit_to_shell) sys_gui_close();
    else               sys_logout();
    return 0;
}
