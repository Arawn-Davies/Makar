/*
 * mxsettings.elf -- the centralised desktop Settings app, as a makx client.
 *
 * macOS-System-Settings layout: a left sidebar of categories + a right content
 * pane of grouped rows.  Each panel reimplements its controls inline and drives
 * the underlying mechanism directly -- the ~/.mxrc keys (mxrc.h), the wallpaper
 * protocol (mx_set_wallpaper), SYS_SETMODE, the net syscalls, SYS_SETTIME -- so
 * Settings is the one place to change everything (the standalone mxdisplay/mxnet
 * are subsumed over time).
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"
#include "mxrc.h"

#define RGB GFX_RGB
#define COL_BG    UI_COL_FIELD                 /* content background          */
#define COL_SIDE  GFX_RGB(0x1b,0x22,0x2e)      /* sidebar background          */
#define COL_TEXT  UI_COL_TEXT
#define COL_MUTE  UI_COL_MUTED
#define COL_HDR   GFX_RGB(0x8a,0xc0,0xff)       /* category title             */

#define SIDEBAR_W 160

/* Category sidebar (the order shown top-to-bottom). */
enum { CAT_APPEARANCE, CAT_DISPLAY, CAT_STATUSBAR, CAT_NETWORK,
       CAT_DATETIME, CAT_AUTOSTART, CAT_N };
static const char *const CATS[CAT_N] = {
    "Appearance", "Display", "Status Bar", "Network", "Date & Time", "Autostart",
};

/* ---- shared content-pane helpers ---------------------------------------- */

/* A section heading (muted, small-caps-ish) at (x,y); returns the next y. */
static int section(gfx_surface *s, int x, int y, const char *title)
{
    gfx_str(s, x, y, title, COL_MUTE);
    return y + 18;
}

/* A "coming soon" placeholder body for panels not yet implemented. */
static void stub_body(gfx_surface *s, int x, int y, const char *what)
{
    gfx_str(s, x, y, what, COL_TEXT);
    gfx_str(s, x, y + 18, "(coming soon)", COL_MUTE);
}

/* ---- per-category panels (filled in over the following phases) ----------- */
/* Each draws into the content rect [cx,cy, cw,ch] of the surface `s`, using the
 * frame's ui_ctx `u`.  Phase 1 ships the shell + stubs; later phases replace
 * each stub body with real controls. */

static void panel_appearance(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "WALLPAPER");
    stub_body(s, cx, y, "Desktop background");
}

static void panel_display(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "RESOLUTION");
    stub_body(s, cx, y, "Screen resolution");
}

static void panel_statusbar(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "STATUS BAR WIDGETS");
    stub_body(s, cx, y, "Tray: clock, date, network, CPU/RAM, GPU");
}

static void panel_network(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "NETWORK");
    stub_body(s, cx, y, "DHCP / manual IP, DNS");
}

static void panel_datetime(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "DATE & TIME");
    stub_body(s, cx, y, "Set the system clock");
}

static void panel_autostart(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)u; (void)cw; (void)ch;
    int y = section(s, cx, cy, "STARTUP APPS");
    stub_body(s, cx, y, "Launch apps at login");
}

static void draw_panel(int cat, ui_ctx *u, gfx_surface *s,
                       int cx, int cy, int cw, int ch)
{
    switch (cat) {
    case CAT_APPEARANCE: panel_appearance(u, s, cx, cy, cw, ch); break;
    case CAT_DISPLAY:    panel_display   (u, s, cx, cy, cw, ch); break;
    case CAT_STATUSBAR:  panel_statusbar (u, s, cx, cy, cw, ch); break;
    case CAT_NETWORK:    panel_network   (u, s, cx, cy, cw, ch); break;
    case CAT_DATETIME:   panel_datetime  (u, s, cx, cy, cw, ch); break;
    case CAT_AUTOSTART:  panel_autostart (u, s, cx, cy, cw, ch); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 720, 520, MX_F_RESIZABLE) != 0) return 1;
    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;

    int sel = 0, scroll = 0;             /* selected category + sidebar scroll */
    int g_menu=-1, g_about=0, g_quit=0;
    int first=1, lmx=-1, lmy=-1, lkey=-2;

    while (!c.closed && !g_quit) {
        mx_pump(&c);
        int key = -1, k; while ((k = mx_key(&c)) >= 0) key = k;   /* last key this frame */

        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        int kch=(key!=lkey); lkey=key;
        if(!(first||c.mpressed||c.mreleased||c.rpressed||moved||kch||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        int top=UI_MENUBAR_H;

        /* backgrounds: sidebar (left) + content (right) */
        gfx_fill(s, 0, top, SIDEBAR_W, s->h-top, COL_SIDE);
        gfx_fill(s, SIDEBAR_W, top, s->w-SIDEBAR_W, s->h-top, COL_BG);

        int busy=(g_menu>=0)||g_about;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,busy?-1:key);
        ui_gate g; ui_gate_begin(&u,&g,busy);

        /* left: category list (the sidebar) */
        ui_listbox(&u,s, 6, top+8, SIDEBAR_W-12, s->h-top-16, CATS, CAT_N, &sel, &scroll);

        /* right: header (category name) + the selected panel */
        int cx = SIDEBAR_W + 18, cy = top + 14, cw = s->w - SIDEBAR_W - 36;
        gfx_str(s, cx, cy, CATS[sel], COL_HDR);
        draw_panel(sel, &u, s, cx, cy + 26, cw, s->h - (cy+26) - 10);

        ui_gate_end(&u,&g);

        static const char *al[]={"Makar Settings (mxsettings)","(c) 2026 Arawn Davies  --  MIT","","Centralised desktop settings: appearance, display, status bar,","network, date & time, autostart.","Part of Makar OS."};
        ui_appbar(&u,s,"mxsettings",al,6,0,0,&g_menu,&g_about,&g_quit);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
