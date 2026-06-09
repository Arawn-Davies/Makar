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
#include "img_bmp.h"
#include "img_png.h"
#include "img_ico.h"
#include "mxrc.h"
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
    int          rawkeys;       /* MX_F_RAWKEYS: wants make/break scancodes     */
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

/* Keyboard delivery mode.  The server reads keys with sys_read(0): cooked bytes
 * (0) normally, raw set-1 scancodes (2) while a MX_F_RAWKEYS client (doom) holds
 * focus -- the kernel keeps the mode per focused-task slot, so this is just the
 * server telling the kernel which stream it wants.  Tracked so we only issue the
 * syscall on a transition.  Modals (login/power/passwd) force cooked. */
static int s_kbd_mode = 0;
static void kbd_mode(int m){ if (m != s_kbd_mode){ sys_keyboard_raw(m); s_kbd_mode = m; } }

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

/* ===================== desktop icons (.desktop shortcuts) ================ */
/* Each desktop icon is an XFCE-style ".desktop" shortcut: Name + Icon (artwork
 * basename or absolute path) + Exec (the client *.elf), plus Makar extensions
 * for the launch window size (X-Makar-WinW/H), an optional extra argv
 * (X-Makar-Arg), the dock/glyph tint (X-Makar-Tint) and the grid position
 * (X-Makar-IconX/Y).  At startup we scan the system-wide /usr/share/shortcuts
 * plus the user overlay ~/.shortcuts (which overrides by filename); if neither
 * has any entries (a stripped image) the built-in default set below is used so
 * the desktop is never empty.  Icons are draggable -- a drop rewrites the
 * X-Makar-IconX/Y back into the source .desktop (best-effort; a silent no-op on
 * a read-only live ISO).  Click selects (highlight); a no-move click launches. */
typedef struct {
    int x, y, w, h;         /* desktop grid cell                              */
    gfx_u32 tint;           /* dock/glyph accent                              */
    int winw, winh;         /* launch window outer size                       */
    char label[24];         /* Name=                                          */
    char cmd[80];           /* Exec= (first token)                            */
    char arg[24];           /* X-Makar-Arg= (optional)                        */
    char icon[40];          /* Icon= (artwork basename or absolute path)      */
    char src[112];          /* source .desktop path ("" = built-in default)   */
} icon_t;
#define ICON_MAX 24
static icon_t icons[ICON_MAX];
static int    g_icon_n = 0;

/* Built-in default app set (used when no .desktop files are present).  winw/winh
 * are sized so each client's fixed surface fits the window client rect 1:1. */
typedef struct { const char *label; gfx_u32 tint; const char *cmd; int winw, winh; const char *arg; const char *icon; } icon_def_t;
static const icon_def_t icon_defs[] = {
    {"Terminal",RGB(0x4c,0x8d,0xff),"/apps/mxterm.elf",   648,424, 0,        "terminal"},
    {"Files",   RGB(0xf0,0xa8,0x30),"/apps/mxfiles.elf",  568,404, 0,        "files"},
    {"Editor",  RGB(0x35,0xc7,0x59),"/apps/mxedit.elf",   628,444, 0,        "editor"},
    {"Tasks",   RGB(0x9b,0x6c,0xff),"/apps/mxtasks.elf",  568,384, 0,        "tasks"},
    {"Doom",    RGB(0xc0,0x40,0x40),"/apps/doom.elf",     648,424, 0,        "doom"},
    {"About",   RGB(0x35,0x6a,0xa8),"/apps/mxabout.elf",  568,454, 0,        "about"},
    {"Clock",   RGB(0x40,0xc0,0xb0),"/apps/mxclock.elf",  384,232, 0,        "clock"},
    {"Calc",    RGB(0xe0,0x80,0x40),"/apps/mxcalc.elf",   264,324, 0,        "calc"},
    {"Net",     RGB(0x4c,0xb0,0xff),"/apps/mxnet.elf",    468,344, 0,        "net"},
    {"Disk",    RGB(0xc0,0xa0,0x40),"/apps/mxdisk.elf",   528,384, 0,        "disk"},
    {"Install", RGB(0xff,0x70,0x70),"/apps/mxinstall.elf",588,492, "install","install"},
    {"Image",   RGB(0x70,0xb0,0x70),"/apps/mximg.elf",    608,468, 0,        "image"},
    {"Display", RGB(0x60,0x90,0xc0),"/apps/mxdisplay.elf",380,300, 0,        "display"},
};
#define ICON_DEF_N (int)(sizeof icon_defs / sizeof icon_defs[0])

static gfx_surface icon_surf[ICON_MAX];
static int         icon_has[ICON_MAX];
static int         g_sel_icon  = -1;   /* clicked/selected icon (highlight)   */
static int         g_hover_icon= -1;   /* icon under the pointer (hover tint)  */

/* small string helpers (freestanding -- no libc) */
static int  wstreq(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return a[i]==0&&b[i]==0; }
static int  wendswith(const char *s, const char *suf){ int n=slen(s),m=slen(suf); return n>=m && wstreq(s+n-m,suf); }
static int  wstrle(const char *a, const char *b){ int i=0; while(a[i]&&a[i]==b[i])i++; return (unsigned char)a[i]<=(unsigned char)b[i]; }
static int  watoi(const char *s){ int v=0,neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;} return neg?-v:v; }
static gfx_u32 whex(const char *s){ unsigned v=0; for(int i=0;i<6&&s[i];i++){ char c=s[i]; int d=(c>='0'&&c<='9')?c-'0':(((c|32)>='a'&&(c|32)<='f')?(c|32)-'a'+10:0); v=(v<<4)|(unsigned)d; } return v&0xFFFFFFu; }
static int  iabs(int v){ return v<0?-v:v; }

/* Two-column desktop grid: col x = 24 / 128, rows step 84. */
static void icon_grid_pos(int idx, int *x, int *y){ int col=idx&1, row=idx>>1; *x=24+col*104; *y=40+row*84; }

/* Resolve and load one icon's artwork.  `spec` is an absolute path (loaded by
 * its extension, or probed if none) or a basename resolved under
 * /usr/share/icons/makar with .ico -> .png -> .bmp probing. */
static int load_one_icon(const char *spec, gfx_surface *out)
{
    char base[160];
    if (spec[0] == '/') {
        scpy(base, spec, sizeof base);
        if (wendswith(base,".ico")) return ico_load(base, out);
        if (wendswith(base,".png")) return png_load(base, out);
        if (wendswith(base,".bmp")) return bmp_load(base, out);
    } else {
        int n=0; const char *pre="/usr/share/icons/makar/";
        for (const char *p=pre; *p; p++) base[n++]=*p;
        for (const char *p=spec; *p && n<(int)sizeof base-1; p++) base[n++]=*p;
        base[n]=0;
    }
    const char *exts[3] = {".ico",".png",".bmp"};
    for (int e=0; e<3; e++){
        char path[176]; scpy(path, base, sizeof path);
        int n=slen(path); scpy(path+n, exts[e], (int)sizeof path - n);
        int rc = (e==0) ? ico_load(path,out) : (e==1) ? png_load(path,out) : bmp_load(path,out);
        if (rc==0) return 0;
    }
    return -1;
}

static void load_icon_assets(void)
{
    for (int i = 0; i < g_icon_n; i++)
        icon_has[i] = (load_one_icon(icons[i].icon, &icon_surf[i]) == 0);
}

/* Parse one .desktop file into *c.  Returns 0 if it carries an Exec=. */
static int parse_desktop_file(const char *path, icon_t *c)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[4096];
    int n=0; long r;
    while (n < (int)sizeof buf - 1 && (r = sys_read(fd, buf+n, (unsigned)((int)sizeof buf-1-n))) > 0) n += (int)r;
    sys_close(fd); buf[n]=0;

    for (unsigned b=0; b<sizeof *c; b++) ((unsigned char*)c)[b]=0;
    c->x=-1; c->y=-1; c->w=96; c->h=70; c->winw=480; c->winh=360; c->tint=RGB(0x40,0x60,0x90);

    int i=0;
    while (i<n) {
        int s=i; while (i<n && buf[i]!='\n' && buf[i]!='\r') i++; buf[i]=0;
        char *line=buf+s; i++; while (i<n && (buf[i]=='\n'||buf[i]=='\r')) i++;
        if (line[0]=='#' || line[0]=='[' || line[0]==0) continue;
        char *eq=line; while (*eq && *eq!='=') eq++; if (*eq!='=') continue; *eq=0;
        char *key=line, *val=eq+1;
        if      (wstreq(key,"Name"))          scpy(c->label, val, sizeof c->label);
        else if (wstreq(key,"Exec"))        { char *sp=val; while (*sp && *sp!=' ') sp++; *sp=0; scpy(c->cmd, val, sizeof c->cmd); }
        else if (wstreq(key,"Icon"))          scpy(c->icon, val, sizeof c->icon);
        else if (wstreq(key,"X-Makar-Arg"))   scpy(c->arg, val, sizeof c->arg);
        else if (wstreq(key,"X-Makar-WinW"))  c->winw = watoi(val);
        else if (wstreq(key,"X-Makar-WinH"))  c->winh = watoi(val);
        else if (wstreq(key,"X-Makar-IconX")) c->x = watoi(val);
        else if (wstreq(key,"X-Makar-IconY")) c->y = watoi(val);
        else if (wstreq(key,"X-Makar-Tint"))  c->tint = whex(val);
    }
    return c->cmd[0] ? 0 : -1;
}

static void add_default_icons(void)
{
    g_icon_n=0;
    for (int i=0; i<ICON_DEF_N && g_icon_n<ICON_MAX; i++){
        icon_t *c=&icons[g_icon_n];
        for (unsigned b=0; b<sizeof *c; b++) ((unsigned char*)c)[b]=0;
        icon_grid_pos(g_icon_n, &c->x, &c->y); c->w=96; c->h=70;
        c->tint=icon_defs[i].tint; c->winw=icon_defs[i].winw; c->winh=icon_defs[i].winh;
        scpy(c->label, icon_defs[i].label, sizeof c->label);
        scpy(c->cmd,   icon_defs[i].cmd,   sizeof c->cmd);
        if (icon_defs[i].arg) scpy(c->arg, icon_defs[i].arg, sizeof c->arg);
        scpy(c->icon,  icon_defs[i].icon,  sizeof c->icon);
        g_icon_n++;
    }
}

/* ~/.mxrc + home resolution live in the shared mxrc module (mxrc.h). */

/* Load an image by extension into `out` (wallpaper-sized bound). */
static int load_image_any(const char *path, gfx_surface *out)
{
    if (wendswith(path,".png")) return png_load(path, out);
    if (wendswith(path,".ico")) return ico_load(path, out);
    if (wendswith(path,".bmp")) return bmp_load_max(path, out, (int)FBW, (int)FBH);
    if (bmp_load_max(path, out, (int)FBW, (int)FBH)==0) return 0;
    return png_load(path, out);
}

/* Desktop wallpaper: path comes from ~/.mxrc (Wallpaper=...).  Blitted stretched
 * behind the icons; empty/missing -> the flat COL_DESK fill.  apply_wallpaper()
 * is cheap when unchanged (re-reads the tiny .mxrc, only reloads on a new path)
 * so the WM can poll it for live "set as wallpaper" updates. */
static gfx_surface g_wallpaper; static int g_has_wp=0; static char g_wp_path[160]={0};
static int g_wp_src=0;     /* 0 none, 1 file (mmap'd px), 2 shared surface       */
static int g_wp_sid=-1;    /* surface id when g_wp_src==2                          */

/* Release the current wallpaper backing (mmap or shared surface). */
static void wp_clear(void)
{
    if (!g_has_wp) return;
    if (g_wp_src==1) sys_munmap(g_wallpaper.px, (unsigned long)g_wallpaper.w*g_wallpaper.h*4u);
    else if (g_wp_src==2 && g_wp_sid>=0) sys_surface_unmap(g_wp_sid);
    g_has_wp=0; g_wp_src=0; g_wp_sid=-1; g_wallpaper.px=0;
}

/* Startup + ~1s-poll path: load the persisted wallpaper from ~/.mxrc (file).
 * A live surface wallpaper (set this session via MX_WALLPAPER) takes precedence
 * and is never clobbered by the poll. */
static int apply_wallpaper(void)
{
    if (g_wp_src==2) return 0;                            /* live surface wins */
    char wp[160]={0};
    if (mxrc_get("Wallpaper", wp, sizeof wp)!=0) wp[0]=0;
    if (wstreq(wp, g_wp_path)) return 0;                  /* unchanged */
    scpy(g_wp_path, wp, sizeof g_wp_path);
    wp_clear();
    if (wp[0] && load_image_any(wp, &g_wallpaper)==0){ g_has_wp=1; g_wp_src=1; }
    return 1;
}

/* Merge the *.desktop shortcuts in `dir` into tmp[]/names[] (count *cnt), with
 * basename override: a shortcut whose filename already collected is replaced
 * in place (so ~/.shortcuts entries override the system-wide ones by name). */
static void merge_shortcuts(const char *dir, icon_t *tmp, char names[][64], int *cnt)
{
    struct dirent de;
    for (unsigned idx=0; idx<4096; idx++){
        int rc=sys_readdir(dir, idx, &de);
        if (rc!=1) break;
        if (de.d_type==DT_DIR) continue;
        if (!wendswith(de.d_name, ".desktop")) continue;
        char path[160]; int p=0;
        for (const char *q=dir; *q; q++) path[p++]=*q; path[p++]='/';
        for (int k=0; de.d_name[k] && p<(int)sizeof path-1; k++) path[p++]=de.d_name[k];
        path[p]=0;
        icon_t e;
        if (parse_desktop_file(path, &e)!=0) continue;
        scpy(e.src, path, sizeof e.src);
        int slot=-1;
        for (int j=0; j<*cnt; j++) if (wstreq(names[j], de.d_name)){ slot=j; break; }
        if (slot<0){ if (*cnt>=ICON_MAX) continue; slot=(*cnt)++; }
        tmp[slot]=e; scpy(names[slot], de.d_name, sizeof names[slot]);
    }
}

/* Build the desktop icon set from the system-wide /usr/share/shortcuts plus the
 * user-local ~/.shortcuts overlay (overrides by filename), sorted by filename
 * for a stable layout, falling back to the built-in defaults if empty. */
static void load_desktop_entries(void)
{
    static icon_t tmp[ICON_MAX]; static char names[ICON_MAX][64];
    int cnt=0;
    merge_shortcuts("/usr/share/shortcuts", tmp, names, &cnt);
    /* ~/.shortcuts user overlay (overrides system-wide by filename). */
    char home[96]; mxrc_home("/.shortcuts", home, sizeof home);
    merge_shortcuts(home, tmp, names, &cnt);
    if (cnt==0){ add_default_icons(); return; }
    /* insertion sort by filename for a deterministic layout */
    for (int a=1; a<cnt; a++){
        icon_t t=tmp[a]; char nm[64]; scpy(nm, names[a], sizeof nm);
        int b=a-1;
        while (b>=0 && !wstrle(names[b], nm)){ tmp[b+1]=tmp[b]; scpy(names[b+1], names[b], sizeof names[b+1]); b--; }
        tmp[b+1]=t; scpy(names[b+1], nm, sizeof names[b+1]);
    }
    for (int i=0; i<cnt; i++){
        if (tmp[i].x<0 || tmp[i].y<0) icon_grid_pos(i, &tmp[i].x, &tmp[i].y);
        icons[i]=tmp[i];
    }
    g_icon_n=cnt;
}

/* Best-effort: rewrite a dragged icon's position back into its .desktop file.
 * Built-in defaults (src=="") and a read-only live ISO are silent no-ops. */
static char *wcat(char *p, const char *s){ while (*s) *p++=*s++; return p; }
static char *wcatint(char *p, int v){ char b[12]; if (v<0){*p++='-'; v=-v;} u2s((unsigned)v, b); return wcat(p, b); }
static char *wcathex(char *p, gfx_u32 v){ const char *h="0123456789abcdef"; for (int i=20; i>=0; i-=4) *p++=h[(v>>i)&0xf]; return p; }
static void icon_save_pos(int ii)
{
    icon_t *c=&icons[ii];
    if (!c->src[0]) return;
    static char out[640]; char *p=out;
    p=wcat(p,"[Desktop Entry]\nType=Application\nName="); p=wcat(p,c->label);
    p=wcat(p,"\nIcon="); p=wcat(p,c->icon);
    p=wcat(p,"\nExec="); p=wcat(p,c->cmd);
    if (c->arg[0]){ p=wcat(p,"\nX-Makar-Arg="); p=wcat(p,c->arg); }
    p=wcat(p,"\nX-Makar-WinW="); p=wcatint(p,c->winw);
    p=wcat(p,"\nX-Makar-WinH="); p=wcatint(p,c->winh);
    p=wcat(p,"\nX-Makar-Tint="); p=wcathex(p,c->tint);
    p=wcat(p,"\nX-Makar-IconX="); p=wcatint(p,c->x);
    p=wcat(p,"\nX-Makar-IconY="); p=wcatint(p,c->y);
    p=wcat(p,"\n"); *p=0;
    int fd=sys_open(c->src, O_WRONLY|O_CREAT|O_TRUNC);
    if (fd<0) return;
    sys_write(fd, out, (unsigned)(p-out));
    sys_close(fd);
}

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
        char *av[5]={ (char*)icons[ii].cmd, "-makx", pids, 0, 0 };
        if (icons[ii].arg[0]) av[3]=(char*)icons[ii].arg;   /* e.g. Install -> "install" */
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

/* Launch an arbitrary client command (used by MX_OPEN: open a file in its
 * default app).  Same fork/pipe/exec dance as launch_icon, but with no
 * desktop-icon association -- the opened window is reaped normally because the
 * server is its parent.  `arg`, if set, is passed as the client's first
 * positional argument (a file path, or the command for a terminal). */
static void launch_cmd(const char *cmd, const char *arg, const char *title, int winw, int winh)
{
    int i=win_alloc(); if(i<0) return;
    int op[2];
    if (sys_pipe(op)<0){ W[i].in_use=0; return; }
    int pid=sys_fork();
    if (pid<0){ sys_close(op[0]); sys_close(op[1]); W[i].in_use=0; return; }
    if (pid==0){
        sys_close(op[0]);
        sys_dup2(op[1],0); sys_dup2(op[1],1); sys_dup2(op[1],2);
        sys_close(op[1]);
        char pids[12]; u2s((unsigned)server_pid, pids);
        char *av[5]={ (char*)cmd, "-makx", pids, 0, 0 };
        if (arg && arg[0]) av[3]=(char*)arg;
        sys_execve(cmd, av, (char *const*)0);
        sys_exit(127);
    }
    sys_close(op[1]);
    sys_fcntl(op[0], F_SETFL, O_NONBLOCK);
    W[i].client=pid; W[i].out=op[0]; W[i].icon=-1; W[i].sid=-1;
    scpy(W[i].title, title?title:"App", sizeof W[i].title);
    W[i].w=winw; W[i].h=winh;
    W[i].x=120+(i*30)%220; W[i].y=MENU_H+24+(i*26)%150;
    z_raise(i); set_focus(i); g_dirty=1; damage_full();
}

/* case-insensitive suffix match (".png" etc.) */
static int ext_is(const char *path, const char *ext){
    int n=0; while(path[n]) n++; int e=0; while(ext[e]) e++;
    if (n<e) return 0;
    const char *p=path+n-e;
    for (int i=0;i<e;i++){ char a=p[i]; if(a>='A'&&a<='Z') a=(char)(a+32); if(a!=ext[i]) return 0; }
    return 1;
}

/* Default-app dispatch (file associations): pick the app for a file type and
 * launch it.  Images -> mximg, html -> mxweb, *.elf GUI apps run directly,
 * other executables in a terminal, everything else in the editor. */
static void wm_open_path(const char *path){
    if (!path || !path[0]) return;
    if (ext_is(path,".png")||ext_is(path,".bmp")||ext_is(path,".jpg")||
        ext_is(path,".jpeg")||ext_is(path,".gif"))
        { launch_cmd("/apps/mximg.elf", path, "Image", 608,468); return; }
    if (ext_is(path,".htm")||ext_is(path,".html"))
        { launch_cmd("/apps/mxweb.elf", path, "Web", 700,500); return; }
    if (ext_is(path,".elf")){
        /* a makx GUI app (mx-prefixed, gui, doom) connects to the server itself
         * -> run it directly; any other executable is a CLI tool -> terminal. */
        const char *base=path; for(const char*q=path;*q;q++) if(*q=='/') base=q+1;
        int gui = (base[0]=='m'&&base[1]=='x') || wstreq(base,"gui.elf") || wstreq(base,"doom.elf");
        if (gui) launch_cmd(path, 0, base, 560,400);
        else     launch_cmd("/apps/mxterm.elf", path, "Terminal", 648,424);
        return;
    }
    launch_cmd("/apps/mxedit.elf", path, "Editor", 628,444);
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
            /* Match a window for this client: a reserved slot (sid<0) OR a
             * re-HELLO from the same pid -- e.g. a launcher that execve'd into
             * the real app (mxdoom -> doom) keeps the WM-launched pid, so the
             * app is still reaped and its window closes on exit. */
            for(int k=0;k<MAXWIN;k++) if(W[k].in_use && W[k].client==src){ i=k; break; }
            if (i<0){ /* a client we didn't reserve: give it a default window
                       * (e.g. doom auto-connecting from a GUI terminal).  Size a
                       * game (MX_F_RAWKEYS) enlarged + with chrome, like the
                       * re-HELLO refit below; everything else gets its requested
                       * size. */
                i=win_alloc();
                if (i>=0){ W[i].client=src;
                    int ww=(w<160?160:w), wh=(h<120?120:h);
                    if (flags & MX_F_RAWKEYS){
                        int availw=(int)FBW-6, availh=(int)FBH-DOCK_H-MENU_H-6;
                        int sc=1; while ((w*(sc+1))<=availw && (h*(sc+1))<=availh) sc++;
                        ww=w*sc+2; wh=h*sc+TH+1;
                    }
                    W[i].w=ww; W[i].h=wh;
                    W[i].x=140; W[i].y=MENU_H+40; scpy(W[i].title,"App",sizeof W[i].title); }
            }
            if (i>=0 && W[i].sid>=0){            /* re-HELLO: drop the old surface, refit window */
                sys_surface_unmap(W[i].sid); sys_surface_destroy(W[i].sid);
                W[i].sid=-1; W[i].surf.px=0;
                /* A game (MX_F_RAWKEYS, e.g. doom re-HELLO'ing after mxdoom
                 * execve'd into it) renders a fixed-size frame the compositor
                 * scales: open it enlarged (the largest integer multiple of its
                 * native frame that fits the desktop) so it doesn't sit tiny. */
                int ww=w+2, wh=h+TH+1;
                if (flags & MX_F_RAWKEYS){
                    int availw=(int)FBW-6, availh=(int)FBH-DOCK_H-MENU_H-6;
                    int sc=1; while ((w*(sc+1))<=availw && (h*(sc+1))<=availh) sc++;
                    ww=w*sc+2; wh=h*sc+TH+1;
                }
                W[i].w=ww; W[i].h=wh;
                if(W[i].x+W[i].w>(int)FBW) W[i].x=(int)FBW-W[i].w; if(W[i].x<0) W[i].x=0;
                if(W[i].y+W[i].h>(int)FBH-DOCK_H) W[i].y=(int)FBH-DOCK_H-W[i].h; if(W[i].y<MENU_H) W[i].y=MENU_H;
            }
            int sid = (i>=0) ? sys_surface_create(w,h) : -1;
            void *base = (sid>=0) ? sys_surface_map(sid) : 0;
            if (i<0 || sid<0 || !base){
                if (sid>=0) sys_surface_destroy(sid);
                r.data[0]=(unsigned)-1; r.data[1]=(unsigned)-1;
            } else {
                W[i].sid=sid; W[i].surf.px=(gfx_u32*)base; W[i].surf.w=w; W[i].surf.h=h; W[i].sw=w; W[i].sh=h;
                W[i].resizable = (flags & MX_F_RESIZABLE) ? 1 : 0;
                W[i].rawkeys   = (flags & MX_F_RAWKEYS)   ? 1 : 0;
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
        } else if (m.type==MX_WALLPAPER){
            /* A client (mximg) handed us a decoded wallpaper as a shared surface
             * (X11 root-pixmap style): map it and blit it behind the icons.  No
             * file read -- immune to cross-process FS-cache coherence. */
            int sid=(int)m.data[0], w=(int)m.data[1], h=(int)m.data[2];
            void *base = (sid>=0 && w>0 && h>0) ? sys_surface_map(sid) : 0;
            if (base){
                wp_clear();
                g_wallpaper.px=(gfx_u32*)base; g_wallpaper.w=w; g_wallpaper.h=h;
                g_has_wp=1; g_wp_src=2; g_wp_sid=sid;
                g_wp_path[0]=0;                       /* poll won't fight the surface */
                g_dirty=1; damage_full();
            }
            r.type=MXEV_NONE;
        } else if (m.type==MX_OPEN){
            /* A client asked us to open a file in its default app.  The path
             * arrives in a throwaway shared surface (it doesn't fit in the IPC
             * payload); map it, copy the path out, then dispatch + launch.  The
             * server is the launcher so the opened window is its child -> reaped
             * normally (a client-forked grandchild would ghost). */
            int sid=(int)m.data[0], len=(int)m.data[1];
            if (len<0) len=0; if (len>255) len=255;
            char path[256];
            unsigned char *pb = (sid>=0) ? (unsigned char*)sys_surface_map(sid) : 0;
            if (pb){
                for (int k=0;k<len;k++) path[k]=(char)pb[k];
                path[len]=0;
                sys_surface_unmap(sid);
                wm_open_path(path);
            }
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
        int st; if(sys_wait4(W[i].client,&st,WNOHANG)==W[i].client){ W[i].client=-1; win_free(i); changed=1; continue; }
        /* A client we didn't fork (e.g. doom launched from a GUI terminal, so
         * it's the terminal shell's child, not ours) can't be reaped via wait4.
         * Probe its liveness with kill(pid,0); when it's gone, close its window
         * -- otherwise it would ghost forever (the server has no socket EOF). */
        if(W[i].out<0 && sys_kill(W[i].client,0)!=0){ W[i].client=-1; win_free(i); changed=1; }
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
        } else {
            /* fixed-size client (e.g. doom): scale the surface to fill the window
             * preserving aspect ratio, centred, with black letterbox/pillarbox
             * bars -- so the game tracks the window size (drag-resize, maximize)
             * instead of sitting tiny in the corner.  1:1 only when it already
             * matches exactly (no scaling cost). */
            if (cw==W[i].sw && ch==W[i].sh){
                gfx_blit(&scr, cx, cy, &W[i].surf, 0,0, W[i].sw, W[i].sh);
            } else {
                long sw=W[i].sw, sh=W[i].sh;
                long dw=cw, dh=cw*sh/sw;
                if (dh>ch){ dh=ch; dw=ch*sw/sh; }
                if (dw<1) dw=1; if (dh<1) dh=1;
                int ox=cx+(cw-(int)dw)/2, oy=cy+(ch-(int)dh)/2;
                gfx_fill(&scr, cx, cy, cw, ch, RGB(0,0,0));    /* letterbox */
                gfx_blit_scaled(&scr, ox, oy, (int)dw, (int)dh, &W[i].surf);
            }
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
    case 6: /* Clock: round face with hour/minute hands */
        gfx_round(&scr,gx+2,gy,28,26,RGB(0x10,0x2a,0x26),bg);
        gfx_outline(&scr,gx+2,gy,28,26,RGB(0x40,0xc0,0xb0));
        gfx_fill(&scr,gx+15,gy+5,2,9,0xFFFFFF);          /* minute hand (up) */
        gfx_fill(&scr,gx+16,gy+12,7,2,0xFFFFFF);         /* hour hand (right) */
        gfx_fill(&scr,gx+15,gy+12,3,3,RGB(0x40,0xc0,0xb0)); /* hub */
        break;
    case 7: /* Calc: display over a 3x3 keypad */
        gfx_round(&scr,gx+3,gy,26,26,RGB(0x20,0x24,0x2c),bg);
        gfx_fill(&scr,gx+6,gy+3,20,6,RGB(0x8a,0xe2,0x34)); /* display */
        for(int r=0;r<3;r++) for(int k=0;k<3;k++)
            gfx_fill(&scr,gx+6+k*7,gy+12+r*5,5,3,RGB(0xe0,0x80,0x40));
        break;
    case 8: /* Net: ascending signal bars */
        gfx_round(&scr,gx,gy,32,26,RGB(0x10,0x20,0x38),bg);
        gfx_fill(&scr,gx+6, gy+15,5,7, RGB(0x4c,0xb0,0xff));
        gfx_fill(&scr,gx+14,gy+10,5,12,RGB(0x4c,0xb0,0xff));
        gfx_fill(&scr,gx+22,gy+5, 5,17,RGB(0x4c,0xb0,0xff));
        break;
    case 9: /* Disk: drive body with platter + spindle */
        gfx_round(&scr,gx,gy,32,26,RGB(0x20,0x1c,0x10),bg);
        gfx_outline(&scr,gx+4,gy+4,24,18,RGB(0xc0,0xa0,0x40));
        gfx_round(&scr,gx+9,gy+7,10,10,RGB(0x80,0x6a,0x28),RGB(0x20,0x1c,0x10));
        gfx_fill(&scr,gx+13,gy+11,3,3,RGB(0xc0,0xa0,0x40)); /* spindle */
        gfx_fill(&scr,gx+22,gy+16,3,3,RGB(0xc0,0xa0,0x40)); /* corner screw */
        break;
    case 10: /* Install: download arrow into a tray */
        gfx_round(&scr,gx,gy,32,26,RGB(0x30,0x16,0x16),bg);
        gfx_fill(&scr,gx+14,gy+3, 4,9, RGB(0xff,0xc0,0xc0)); /* shaft */
        gfx_fill(&scr,gx+10,gy+11,12,2,RGB(0xff,0xc0,0xc0)); /* arrowhead */
        gfx_fill(&scr,gx+12,gy+13,8, 2,RGB(0xff,0xc0,0xc0));
        gfx_fill(&scr,gx+14,gy+15,4, 2,RGB(0xff,0xc0,0xc0));
        gfx_fill(&scr,gx+5, gy+20,22,3,RGB(0xff,0x70,0x70)); /* tray */
        break;
    case 11: /* Image: framed picture with sun and hill */
        gfx_round(&scr,gx,gy,32,26,RGB(0x10,0x22,0x12),bg);
        gfx_outline(&scr,gx+3,gy+2,26,22,RGB(0x70,0xb0,0x70));
        gfx_fill(&scr,gx+8,gy+6,5,5,RGB(0xff,0xe0,0x60));    /* sun */
        gfx_fill(&scr,gx+5,gy+15,22,7,RGB(0x40,0x90,0x50));  /* hill/ground */
        break;
    default:
        gfx_round(&scr,gx,gy,32,26,RGB(0x4c,0x8d,0xff),bg);
        break;
    }
}
static void draw_icons(void)
{
    for(int i=0;i<g_icon_n;i++){ icon_t *c=&icons[i];
        /* selection / hover highlight: a rounded plate behind the icon tile
         * (selected = brighter blue + outline, hover = subtle lift). */
        if (i==g_sel_icon || i==g_hover_icon){
            gfx_u32 hl = (i==g_sel_icon) ? RGB(0x35,0x4f,0x78) : RGB(0x26,0x33,0x49);
            gfx_round(&scr,c->x-3,c->y-3,c->w+6,c->h+6,hl,COL_DESK);
            if (i==g_sel_icon) gfx_outline(&scr,c->x-3,c->y-3,c->w+6,c->h+6,RGB(0x5a,0x86,0xcc));
        }
        gfx_round(&scr,c->x,c->y,c->w,c->h,RGB(0x2a,0x38,0x50),COL_DESK);
        int gx=c->x+c->w/2-16, gy=c->y+9;
        if (icon_has[i]){
            if (icon_surf[i].w==32 && icon_surf[i].h==26)
                gfx_blit(&scr,gx,gy,&icon_surf[i],0,0,32,26);
            else
                gfx_blit_scaled(&scr,gx,gy,32,26,&icon_surf[i]);
        } else {
            icon_glyph(i, gx, gy);
        }
        gfx_str(&scr,c->x+(c->w-gfx_text_w(c->label))/2,c->y+c->h-16,c->label,0xFFFFFF);
    }
}
static int icon_hit(int px,int py){ for(int i=0;i<g_icon_n;i++){icon_t*c=&icons[i]; if(in_rect(px,py,c->x,c->y,c->w,c->h)) return i;} return -1; }

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

/* ---- system tray: net status + clock/date (lives in the dock, far right) - */
static int      g_net_state = -1;        /* 0 down, 1 limited, 2 connected   */
/* Dock tray element visibility (toggled via the dock right-click menu, persisted
 * in ~/.mxrc).  Default all on. */
static int g_tray_clock=1, g_tray_date=1, g_tray_net=1, g_tray_stats=1, g_tray_gpu=1;
static char g_gpu_name[20] = "";          /* active video backend (queried once)  */
static void load_tray_prefs(void){
    g_tray_clock = mxrc_get_int("TrayClock", 1);
    g_tray_date  = mxrc_get_int("TrayDate",  1);
    g_tray_net   = mxrc_get_int("TrayNet",   1);
    g_tray_stats = mxrc_get_int("TrayStats", 1);
    g_tray_gpu   = mxrc_get_int("TrayGpu",   1);
    if (sys_video_name(g_gpu_name, sizeof g_gpu_name) <= 0) scpy(g_gpu_name, "VGA", sizeof g_gpu_name);
}
static char     g_clk[8]   = "--:--";    /* HH:MM                            */
static char     g_date[10] = "--/--/--"; /* DD/MM/YY                         */
static unsigned g_tray_sec = 0xffffffffu;/* last poll second (uptime/100)    */

static void tray_poll(void)
{
    unsigned sec = sys_uptime() / 100u;          /* 100 Hz ticks -> seconds  */
    if (sec == g_tray_sec) return;               /* poll at most once/second */
    g_tray_sec = sec;
    g_net_state = sys_net_ctl(NET_CTL_STATUS);
    if (g_net_state < 0) g_net_state = 0;
    /* /proc/rtc = "YYYY-MM-DD HH:MM:SS" -> HH:MM and DD/MM/YY. */
    char b[40];
    int fd = sys_open("/proc/rtc", O_RDONLY);
    if (fd >= 0) {
        long n = sys_read(fd, b, (long)sizeof b - 1);
        sys_close(fd);
        if (n >= 19) {
            g_clk[0]=b[11]; g_clk[1]=b[12]; g_clk[2]=':'; g_clk[3]=b[14]; g_clk[4]=b[15]; g_clk[5]=0;
            g_date[0]=b[8]; g_date[1]=b[9]; g_date[2]='/'; g_date[3]=b[5]; g_date[4]=b[6];
            g_date[5]='/'; g_date[6]=b[2]; g_date[7]=b[3]; g_date[8]=0;
        }
    }
}

/* Vista-style network indicator: 4 ascending bars; green = connected,
 * amber = up-but-limited, grey + red X = down / no interface. */
static void draw_net_icon(int x, int y, int state)
{
    unsigned col = state >= 2 ? RGB(0x57,0xc2,0x4d)
                 : state == 1 ? RGB(0xe0,0xb0,0x20)
                              : RGB(0x55,0x60,0x70);
    static const int bh[4] = { 3, 6, 9, 12 };
    for (int i = 0; i < 4; i++)
        gfx_fill(&scr, x + i*4, y + 12 - bh[i], 3, bh[i], col);
    if (state <= 0) {                            /* red X = no connection */
        unsigned r = RGB(0xe0,0x40,0x30);
        for (int k = 0; k <= 8; k++) { gfx_px(&scr,x+3+k,y+2+k,r); gfx_px(&scr,x+11-k,y+2+k,r); }
    }
}

/* dock: one tile per open window (kind/icon order for stable positions) */
static int dock_order[MAXWIN], dock_n;
static void dock_rebuild(void)
{
    dock_n=0;
    for(int ii=0;ii<g_icon_n;ii++) for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].icon==ii) dock_order[dock_n++]=i;
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
    /* status tray (bottom-right -- one place): CPU/RAM, net, clock, date,
     * reading left-to-right "CPU x% RAM y%  [net]  HH:MM  DD/MM/YY". */
    tray_poll();
    unsigned tcol = RGB(0xc8,0xd0,0xdc); int ty = y0+(DOCK_H-8)/2; int rx = (int)FBW-10;
    if (g_tray_date){ rx -= gfx_text_w(g_date); gfx_str(&scr, rx, ty, g_date, tcol); rx -= 12; }
    if (g_tray_clock){ rx -= gfx_text_w(g_clk);  gfx_str(&scr, rx, ty, g_clk,  tcol); rx -= 16; }
    if (g_tray_net){ rx -= 16; draw_net_icon(rx, y0+(DOCK_H-12)/2, g_net_state); rx -= 14; }
    if (g_tray_stats){ char st[32]; dock_stats(st);
        rx -= gfx_text_w(st); gfx_str(&scr, rx, ty, st, RGB(0x90,0xa0,0xb5)); rx -= 12; }
    if (g_tray_gpu && g_gpu_name[0]){
        char gp[28]; int o=0; const char *p="GPU "; while(*p)gp[o++]=*p++;
        for(const char *q=g_gpu_name; *q && o<(int)sizeof gp-1; q++) gp[o++]=*q; gp[o]=0;
        rx -= gfx_text_w(gp); gfx_str(&scr, rx, ty, gp, RGB(0x7a,0xc0,0x90)); }
}
static int dock_hit(int px,int py,int *out_win)
{
    int y0=(int)FBH-DOCK_H; if(py<y0) return 0;
    for(int s=0;s<dock_n;s++){ int bx=dock_btn_x(s); if(in_rect(px,py,bx,y0+5,54,DOCK_H-10)){ *out_win=dock_order[s]; return 1; } }
    return 0;
}

/* ---- dock right-click menu: toggle which tray elements show (persist ~/.mxrc) */
#define TRAYMENU_W 150
#define TRAYMENU_N 5
static const char *TRAY_LABELS[TRAYMENU_N] = {"Clock","Date","Network","CPU / RAM","GPU"};
static const char *TRAY_KEYS[TRAYMENU_N]   = {"TrayClock","TrayDate","TrayNet","TrayStats","TrayGpu"};
static int g_tray_menu=0, g_tray_menu_x=0;     /* open flag + anchor x (pops up from dock) */
static int *tray_flag(int i){ return i==0?&g_tray_clock : i==1?&g_tray_date : i==2?&g_tray_net : i==3?&g_tray_stats : &g_tray_gpu; }
static void tray_menu_box(int *x,int *y,int *w,int *h){
    int rh=22; *w=TRAYMENU_W; *h=6+TRAYMENU_N*rh+6;
    *x=g_tray_menu_x; if(*x+*w>(int)FBW)*x=(int)FBW-*w; if(*x<0)*x=0;
    *y=(int)FBH-DOCK_H-*h;                       /* sit just above the dock */
}
static void draw_tray_menu(void){
    if(!g_tray_menu) return;
    int x,y,w,h,rh=22; tray_menu_box(&x,&y,&w,&h);
    gfx_round(&scr,x,y,w,h,COL_WIN,COL_BORDER);
    for(int i=0;i<TRAYMENU_N;i++){ int ry=y+6+i*rh;
        gfx_str(&scr,x+10,ry+(rh-8)/2, *tray_flag(i)?"x":" ", RGB(0x8a,0xe2,0x34));
        gfx_str(&scr,x+26,ry+(rh-8)/2, TRAY_LABELS[i], 0xFFFFFF);
    }
}
/* Handle a left-click while the menu is open: toggle a row, or close on an
 * outside click.  Returns 1 if the click was consumed (menu was open). */
static int tray_menu_click(int px,int py){
    if(!g_tray_menu) return 0;
    int x,y,w,h,rh=22; tray_menu_box(&x,&y,&w,&h);
    if(in_rect(px,py,x,y,w,h)){
        for(int i=0;i<TRAYMENU_N;i++){ int ry=y+6+i*rh;
            if(in_rect(px,py,x,ry,w,rh)){ int *f=tray_flag(i); *f=!*f; mxrc_set_int(TRAY_KEYS[i],*f); break; }
        }
    }
    g_tray_menu=0;                               /* any click closes the menu */
    return 1;
}

/* top menu bar -------------------------------------------------------------- */
/* IEC 5009 "standby" power glyph (11x11): a broken ring with a vertical bar. */
#define POWER_W 30
static const char *PWR_ICON[11]={
 "    XX     ",
 "  X XX X   ",
 " X  XX  X  ",
 "X   XX   X ",
 "X        X ",
 "X        X ",
 "X        X ",
 " X      X  ",
 "  X    X   ",
 "   XXXX    ",
 "           " };
static void draw_power_icon(int bx,int by,gfx_u32 col)
{
    for(int r=0;r<11;r++) for(int c=0;PWR_ICON[r][c];c++)
        if(PWR_ICON[r][c]=='X') gfx_px(&scr,bx+c,by+r,col);
}

static void draw_menubar(void)
{
    gfx_fill(&scr,0,0,(int)FBW,MENU_H,COL_MENU);
    gfx_fill(&scr,0,MENU_H-1,(int)FBW,1,RGB(0x28,0x32,0x44));
    gfx_str(&scr,8,(MENU_H-8)/2,"Makar",RGB(0x8a,0xe2,0x34));
    gfx_str(&scr,64,(MENU_H-8)/2, (focus>=0&&W[focus].in_use)?W[focus].title:"Desktop", RGB(0x90,0xa0,0xb5));
    /* power button stays top-right; the net/clock/date tray moved to the dock */
    int px0=(int)FBW-POWER_W-4;
    gfx_fill(&scr,px0,2,POWER_W,MENU_H-4,UI_COL_BTN);
    draw_power_icon(px0+(POWER_W-11)/2,(MENU_H-11)/2,0xFFFFFF);
}
static int power_hit(int px,int py){ int x0=(int)FBW-POWER_W-4; return in_rect(px,py,x0,2,POWER_W,MENU_H-4); }

/* ---- mouse cursor ------------------------------------------------------- */
/* Two 11-wide (+NUL) sprites: the arrow and a busy hourglass shown while a
 * client is launching (forked but no surface yet).  'X' = outline, '.' = body,
 * space = transparent.  Rows are 11 chars so the HW-cursor upload's fixed
 * 12-column scan reads the trailing NUL (transparent) without overrunning. */
static const char *CURSOR[16]={
 "X          ","XX         ","X.X        ","X..X       ","X...X      ","X....X     ",
 "X.....X    ","X......X   ","X.......X  ","X........X ","X....XXXXXX","X..X.X     ",
 "X.X X.X    ","XX  X.X    ","X    X.X   ","      XX   " };
static const char *BUSY[16]={
 "XXXXXXXX   ","X......X   "," X....X    ","  X..X     ","   XX      ","   XX      ",
 "  X..X     "," X....X    ","X......X   ","XXXXXXXX   ","           ","           ",
 "           ","           ","           ","           " };
static int g_busy = 0;                  /* 1 while a client is launching */
static void draw_cursor(int cx,int cy)
{
    const char *const *bm = g_busy ? BUSY : CURSOR;
    for(int r=0;r<16;r++) for(int c=0;bm[r][c];c++){ char p=bm[r][c];
        if(p=='X')gfx_px(&scr,cx+c,cy+r,0); else if(p=='.')gfx_px(&scr,cx+c,cy+r,0xFFFFFF); }
}

/* Any window forked but not yet showing a surface -> show the busy cursor. */
static int wm_busy(void)
{
    for(int i=0;i<MAXWIN;i++) if(W[i].in_use && W[i].client>=0 && W[i].sid<0) return 1;
    return 0;
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

/* Hardware cursor: when the display driver advertises one (SVGA II), the WM
 * uploads the arrow sprite once and just moves the HW overlay, skipping the
 * software save-under compositing entirely -- a pure mouse move then costs no
 * framebuffer traffic at all.  0 = software cursor (the universal path). */
static int g_hwcursor = 0;
/* Upload one of the sprites (arrow / busy hourglass) to the HW cursor overlay. */
static void hwcursor_upload(const char *const bm[]){
    static gfx_u32 spr[CURW*CURH];
    for (int r=0;r<CURH;r++) for (int c=0;c<CURW;c++){
        char p = bm[r][c];                     /* row strings are 11+NUL; c=11 -> 0 */
        spr[r*CURW+c] = (p=='X') ? 0xFF000000u      /* outline: opaque black */
                       : (p=='.') ? 0xFFFFFFFFu      /* body: opaque white    */
                       : 0x00000000u;                /* transparent           */
    }
    sys_hwcursor_define(spr, CURW, CURH, 0, 0);
}
static void hwcursor_setup(void){
    if (!(sys_video_caps() & VIDEO_CAP_HW_CURSOR)) return;
    static gfx_u32 spr[CURW*CURH];
    for (int r=0;r<CURH;r++) for (int c=0;c<CURW;c++){
        char p = CURSOR[r][c];                 /* row strings are 11+NUL; c=11 -> 0 */
        spr[r*CURW+c] = (p=='X') ? 0xFF000000u
                       : (p=='.') ? 0xFFFFFFFFu
                       : 0x00000000u;
    }
    if (sys_hwcursor_define(spr, CURW, CURH, 0, 0) == 0){
        g_hwcursor = 1;
        sys_hwcursor_show(1);
    }
}

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

/* Re-initialise the display after a runtime resolution change (mxdisplay calls
 * SYS_SETMODE; the kernel repoints the framebuffer; we notice the new geometry
 * each frame and reflow in place -- no process restart / re-login).  Reallocates
 * the back buffer, clamps/re-fits windows, and recomposites. */
static void wm_reinit_display(unsigned nw, unsigned nh)
{
    gfx_u32 *nb = (gfx_u32*)sys_mmap(0,(unsigned long)nw*nh*4,
                                     PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (nb==(gfx_u32*)MAP_FAILED || !nb) return;     /* keep the old mode on OOM */
    if (scr.px) sys_munmap(scr.px,(unsigned long)FBW*FBH*4);
    scr.px=nb; scr.w=(int)nw; scr.h=(int)nh;
    FBW=nw; FBH=nh;

    for(int i=0;i<MAXWIN;i++){ if(!W[i].in_use) continue;
        if (W[i].maximized){
            W[i].x=0; W[i].y=MENU_H; W[i].w=(int)FBW; W[i].h=(int)FBH-MENU_H-DOCK_H;
            maybe_send_resize(i);
        } else {
            if (W[i].w>(int)FBW) W[i].w=(int)FBW;
            if (W[i].h>(int)FBH-MENU_H-DOCK_H) W[i].h=(int)FBH-MENU_H-DOCK_H;
            if (W[i].x+W[i].w>(int)FBW) W[i].x=(int)FBW-W[i].w;
            if (W[i].x<0) W[i].x=0;
            if (W[i].y+W[i].h>(int)FBH-DOCK_H) W[i].y=(int)FBH-DOCK_H-W[i].h;
            if (W[i].y<MENU_H) W[i].y=MENU_H;
        }
    }
    cur_sx=cur_sy=-1;                 /* SW cursor save-under is stale */
    if (g_hwcursor) sys_hwcursor_show(1);
    g_dirty=1; damage_full();
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
    kbd_mode(0);                 /* modal needs cooked bytes, not scancodes */
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
        if (g_hwcursor) sys_hwcursor_move(cx,cy); else draw_cursor(cx,cy);
        sys_fb_present(scr.px);
        dirty=0; sys_yield();
    }
}

/* ============================ power menu ================================ */
enum { PWR_NONE=0, PWR_CANCEL, PWR_LOGOUT_GUI, PWR_LOGOUT_SHELL,
       PWR_SHUTDOWN, PWR_REBOOT, PWR_PASSWD };

/* Centred modal power menu (clones do_login's input/render loop).  Returns one
 * of the PWR_* actions; Cancel / Esc dismisses it.  The caller (main, or the
 * Ctrl-Alt-Del path) repaints the desktop afterwards. */
static int show_power_menu(int cx, int cy)
{
    int prev_left=0, dirty=1;
    kbd_mode(0);                 /* modal needs cooked bytes, not scancodes */
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;

    static const char *labels[6]={
        "Log out (graphical)","Log out to shell","Shut down",
        "Reboot","Change password...","Cancel" };
    static const int acts[6]={ PWR_LOGOUT_GUI,PWR_LOGOUT_SHELL,PWR_SHUTDOWN,
                               PWR_REBOOT,PWR_PASSWD,PWR_CANCEL };

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
        if(key==27) return PWR_CANCEL;                 /* Esc */
        if(!dirty){ sys_yield(); continue; }

        gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
        int pw=300, ph=44+6*40+12, px=(int)FBW/2-pw/2, py=(int)FBH/2-ph/2;
        gfx_round(&scr,px,py,pw,ph,COL_WIN,COL_BORDER);
        gfx_fill(&scr,px,py,pw,26,COL_TITLE);
        gfx_str(&scr,px+(pw-gfx_text_w("Power"))/2,py+9,"Power",0xFFFFFF);

        ui_begin(&u,cx,cy,mdown,mpressed,mreleased,-1);
        int ret=PWR_NONE;
        for(int i=0;i<6;i++)
            if(ui_button(&u,&scr,px+24,py+40+i*40,pw-48,30,labels[i])) ret=acts[i];

        if (g_hwcursor) sys_hwcursor_move(cx,cy); else draw_cursor(cx,cy);
        sys_fb_present(scr.px);
        dirty=0;
        if(ret!=PWR_NONE) return ret;
        sys_yield();
    }
}

/* Graphical change-password dialog: Current / New / Confirm via masked fields,
 * OK calls sys_passwd (verify old + set new over /etc/shadow).  Tab cycles
 * fields; Esc/Cancel dismisses.  Mirrors do_login's input/render loop. */
static void show_passwd_dialog(int cx, int cy)
{
    char oldp[64]={0}, newp[64]={0}, conf[64]={0}, err[48]={0};
    int prev_left=0, dirty=1;
    kbd_mode(0);                 /* modal needs cooked bytes, not scancodes */
    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    u.focus=1;

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
        if(key==27) return;                                /* Esc cancels */
        if(!dirty){ sys_yield(); continue; }

        gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
        int pw=340, ph=232, px=(int)FBW/2-pw/2, py=(int)FBH/2-ph/2;
        gfx_round(&scr,px,py,pw,ph,COL_WIN,COL_BORDER);
        gfx_fill(&scr,px,py,pw,26,COL_TITLE);
        gfx_str(&scr,px+(pw-gfx_text_w("Change password"))/2,py+9,"Change password",0xFFFFFF);
        gfx_str(&scr,px+24,py+50,"Current:",COL_TEXT);
        gfx_str(&scr,px+24,py+86,"New:",COL_TEXT);
        gfx_str(&scr,px+24,py+122,"Confirm:",COL_TEXT);

        ui_begin(&u,cx,cy,mdown,mpressed,mreleased,
                 (key=='\t'||key=='\n'||key=='\r')?-1:key);
        ui_password(&u,&scr,px+110,py+44, pw-134,22,oldp,(int)sizeof oldp);
        ui_password(&u,&scr,px+110,py+80, pw-134,22,newp,(int)sizeof newp);
        ui_password(&u,&scr,px+110,py+116,pw-134,22,conf,(int)sizeof conf);
        int okc =ui_button(&u,&scr,px+pw/2-92,py+162,88,28,"OK");
        int cnc =ui_button(&u,&scr,px+pw/2+4, py+162,88,28,"Cancel");
        if(err[0]) gfx_str(&scr,px+24,py+ph-22,err,COL_CLOSE);

        if(key=='\t') u.focus = (u.focus>=3)?1:(u.focus+1);
        if(cnc) return;
        if(okc || key=='\n' || key=='\r'){
            int match=1; for(int i=0;;i++){ if(newp[i]!=conf[i]){match=0;break;} if(!newp[i])break; }
            if(!newp[0])      { scpy(err,"New password is empty",sizeof err); u.focus=2; }
            else if(!match)   { scpy(err,"New passwords do not match",sizeof err); conf[0]=0; u.focus=3; }
            else {
                int rc=sys_passwd(oldp,newp);
                if(rc==0) return;                          /* changed -> close */
                else if(rc==-2){ scpy(err,"Current password incorrect",sizeof err); oldp[0]=0; u.focus=1; }
                else           { scpy(err,"Could not change (read-only?)",sizeof err); }
            }
        }
        if (g_hwcursor) sys_hwcursor_move(cx,cy); else draw_cursor(cx,cy);
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

    load_desktop_entries();     /* /usr/share/shortcuts + ~/.shortcuts (or defaults) */
    load_icon_assets();         /* per-icon artwork (.ico/.png/.bmp; glyph fallback) */
    apply_wallpaper();          /* ~/.mxrc Wallpaper= (flat desktop if unset) */
    load_tray_prefs();          /* ~/.mxrc dock tray visibility (default all on) */
    hwcursor_setup();           /* use the display driver's HW cursor if it has one */
    znum=0; focus=-1;
    launch_icon(0);             /* open a terminal client on the desktop */

    int cx=(int)FBW/2, cy=(int)FBH/2, prev_left=0;
    int dragging=0, resizing=0, drag_win=-1, drag_dx=0, drag_dy=0;
    int drag_icon=-1, icon_moved=0, icon_dx=0, icon_dy=0, icon_px=0, icon_py=0;
    int prev_right=0;
    int announced=0, exit_to_shell=0, power_action=0;
    unsigned stat_up=0;

    for(;;){
        /* ---- pick up a runtime resolution change (mxdisplay -> SYS_SETMODE) ---- */
        { unsigned gi=sys_fb_info(); unsigned nw=(gi>>16)&0xFFFF, nh=gi&0xFFFF;
          if (nw && nh && (nw!=FBW || nh!=FBH)){
              wm_reinit_display(nw,nh);
              if (cx>=(int)FBW) cx=(int)FBW-1;
              if (cy>=(int)FBH) cy=(int)FBH-1;
          } }
        /* ---- gather hardware input ---- */
        int mpressed=0, mreleased=0, rpressed=0;
        unsigned int ev;
        while((ev=sys_mouse_read())!=0){
            cx += (int)(signed char)((ev>>8)&0xFF);
            cy += (int)(signed char)((ev>>16)&0xFF);
            if(cx<0)cx=0; if(cx>=(int)FBW)cx=(int)FBW-1;
            if(cy<0)cy=0; if(cy>=(int)FBH)cy=(int)FBH-1;
            int left=ev&1, right=ev&2;
            if(left&&!prev_left) mpressed=1;
            if(!left&&prev_left) mreleased=1;
            if(right&&!prev_right) rpressed=1;
            prev_left=left; prev_right=right;
            /* NB: a pure cursor move does NOT dirty the scene -- it's handled by
             * the cheap cursor-only path below.  Scene changes (clicks, drags,
             * client repaints, focus) set g_dirty in their own handlers. */
        }
        int mdown=prev_left;
        int frame_key=-1;
        /* Match the keyboard stream to the focused client: a MX_F_RAWKEYS game
         * gets the raw make/break scancode stream, everyone else cooked bytes. */
        kbd_mode((focus>=0 && W[focus].in_use && W[focus].rawkeys) ? 2 : 0);
        /* A key only changes the screen via the focused client's repaint (which
         * damages its own window); the WM chrome doesn't render keys, so this
         * doesn't dirty the scene by itself. */
        { unsigned char b; if (sys_read(0,&b,1)==1){ frame_key=b; } }
        int cmoved = (cx!=cur_sx || cy!=cur_sy);  /* cursor moved this frame? */

        /* ---- window-management click handling ---- */
        /* The power menu opens from the menubar icon OR from Ctrl-Alt-Del
         * (polled below) -- both set this so the action dispatch lives once. */
        int want_power_menu = 0;

        /* right-click on the dock opens the tray-visibility menu */
        if (rpressed && cy >= (int)FBH-DOCK_H){ g_tray_menu_x=cx; g_tray_menu=1; g_dirty=1; damage_full(); }

        if (mpressed && g_tray_menu){          /* a click while the menu is open: toggle/close */
            tray_menu_click(cx,cy); g_dirty=1; damage_full();
        } else if (mpressed){
            int dk;
            g_sel_icon=-1;                    /* clear selection unless an icon is hit */
            if (power_hit(cx,cy)) want_power_menu=1;
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
                    if (ii>=0){           /* select + begin a potential drag (launch on release if not moved) */
                        g_sel_icon=ii;
                        drag_icon=ii; icon_moved=0;
                        icon_dx=cx-icons[ii].x; icon_dy=cy-icons[ii].y;
                        icon_px=cx; icon_py=cy;
                    }
                }
            }
        }
        if (mreleased){
            /* On finishing a resize drag, re-flow the client to the new size. */
            if (resizing && drag_win>=0) maybe_send_resize(drag_win);
            dragging=0; resizing=0;
            /* Icon: a plain click (no drag) launches; a drag drops + persists. */
            if (drag_icon>=0){
                if (!icon_moved) launch_icon(drag_icon);
                else            icon_save_pos(drag_icon);
                drag_icon=-1; icon_moved=0;
            }
        }
        /* ---- icon drag: move the desktop icon under the pointer ---- */
        if (drag_icon>=0 && mdown && drag_icon<g_icon_n){
            if (!icon_moved && (iabs(cx-icon_px)>4 || iabs(cy-icon_py)>4)) icon_moved=1;
            if (icon_moved){
                icon_t *c=&icons[drag_icon];
                damage(c->x,c->y,c->w,c->h);          /* erase old cell */
                c->x=cx-icon_dx; c->y=cy-icon_dy;
                if(c->x<0)c->x=0; if(c->y<MENU_H)c->y=MENU_H;
                if(c->x+c->w>(int)FBW)c->x=(int)FBW-c->w;
                if(c->y+c->h>(int)FBH-DOCK_H)c->y=(int)FBH-DOCK_H-c->h;
                g_dirty=1; damage(c->x,c->y,c->w,c->h); /* new cell */
            }
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

        /* ---- desktop icon hover highlight: repaint only when it changes ---- */
        { int hv = (drag_icon<0) ? icon_hit(cx,cy) : -1;
          if (hv != g_hover_icon){
              int prev=g_hover_icon; g_hover_icon=hv; g_dirty=1;
              if (prev>=0) damage(icons[prev].x-3,icons[prev].y-3,icons[prev].w+6,icons[prev].h+6);
              if (hv>=0)   damage(icons[hv].x-3,  icons[hv].y-3,  icons[hv].w+6,  icons[hv].h+6);
          }
        }

        /* ---- Ctrl-Alt-Del opens the power menu instantly (kernel sets the
         * flag from the IRQ; we test-and-clear it every frame) ---- */
        if (sys_cad_pending()) want_power_menu=1;

        if (want_power_menu){
            int act=show_power_menu(cx,cy);
            g_dirty=1; damage_full();                 /* repaint desktop after modal */
            if      (act==PWR_SHUTDOWN)    { power_action=PWR_SHUTDOWN;    break; }
            else if (act==PWR_REBOOT)      { power_action=PWR_REBOOT;      break; }
            else if (act==PWR_LOGOUT_GUI)  { power_action=PWR_LOGOUT_GUI;  break; }
            else if (act==PWR_LOGOUT_SHELL){ exit_to_shell=1; break; }
            else if (act==PWR_PASSWD)      { show_passwd_dialog(cx,cy); g_dirty=1; damage_full(); }
            /* CANCEL: stay on the desktop */
            prev_left=0;                               /* swallow the click that opened it */
        }

        /* ---- forward input to the focused client over IPC ---- */
        if (focus>=0 && W[focus].in_use){
            swin *w=&W[focus];
            if (frame_key>=0) win_push(w, MXEV_KEY, frame_key,0,0);
            /* Deliver pointer motion to the focused client (X11-style): the
             * client tracks the live cursor so its hit-test (`hot`) is correct
             * the instant a button goes down.  Suppressing hover (forwarding
             * only on a button edge/drag) left the client's pointer one event
             * stale, so a click landed on the *previously* known position and
             * needed a second click.  win_push coalesces a run of same-button
             * moves, so this doesn't flood the queue; only the focused window
             * (the one under the pointer) repaints. */
            if (mpressed || mreleased || cmoved){
                if (in_client(w,cx,cy)){
                    int rx=cx-client_x(w), ry=cy-client_y(w);
                    win_push(w, MXEV_MOUSE, rx, ry, mdown?1:0);
                } else if (mreleased || mpressed){
                    win_push(w, MXEV_MOUSE, cx-client_x(w), cy-client_y(w), mdown?1:0);
                }
            }
        }

        /* ---- service clients + reap exited ones ---- */
        serve_requests();
        if (reap_clients()){ g_dirty=1; damage_full(); }
        /* busy cursor: hourglass while any launched client has no surface yet */
        { int busy=wm_busy(); if (busy!=g_busy){ g_busy=busy; g_dirty=1;
            if (g_hwcursor) hwcursor_upload(busy?BUSY:CURSOR);
            else damage(cur_sx<0?cx:cur_sx, cur_sy<0?cy:cur_sy, CURW, CURH); } }
        { unsigned now=sys_uptime(); if (now-stat_up>=100u){ stat_up=now; g_dirty=1;
            damage(0,0,(int)FBW,MENU_H);                       /* top bar title  */
            damage(0,(int)FBH-DOCK_H,(int)FBW,DOCK_H);          /* dock stats+tray */
            if (apply_wallpaper()) damage_full(); } }           /* live "set as wallpaper" */

        if (!g_dirty && !cmoved){ sys_yield(); continue; }

        if (g_dirty){
            /* ---- recompose the WHOLE back buffer (cheap, cacheable RAM) ---- */
            if (g_has_wp) gfx_blit_scaled(&scr,0,0,(int)FBW,(int)FBH,&g_wallpaper);
            else          gfx_fill(&scr,0,0,(int)FBW,(int)FBH,COL_DESK);
            gfx_str(&scr,8,MENU_H+6,"Makar desktop -- click an icon; drag a title bar; click a window to focus",RGB(0x90,0xa0,0xb5));
            draw_icons();
            for (int j=0;j<znum;j++){ int i=zorder[j]; if(!W[i].in_use || W[i].minimized) continue; draw_window_frame(i); }
            draw_dock();
            draw_menubar();
            draw_tray_menu();
            int ox=cur_sx, oy=cur_sy;
            if (!g_hwcursor){ cursor_capture(cx,cy); draw_cursor(cx,cy); }
            /* ---- but PUSH only the damaged region to the framebuffer ---- */
            if (!dmg_v) damage_full();      /* safety net for any untracked change */
            present_rect(dmg_x0,dmg_y0,dmg_x1-dmg_x0,dmg_y1-dmg_y0);
            if (!g_hwcursor){
                if (cmoved && ox>=0) present_rect(ox,oy,CURW,CURH);  /* erase old cursor */
                present_rect(cx,cy,CURW,CURH);                       /* draw new cursor  */
            } else if (cmoved){
                sys_hwcursor_move(cx,cy); cur_sx=cx; cur_sy=cy;      /* HW overlay follows */
            }
            if (!announced){ sys_write_serial("GUI: READY\n", 11); announced=1; }
            g_dirty=0; dmg_v=0;
        } else if (g_hwcursor){
            /* ---- cursor-only with a HW cursor: move the overlay, no FB push ---- */
            sys_hwcursor_move(cx,cy); cur_sx=cx; cur_sy=cy;
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
    kbd_mode(0);   /* hand the keyboard back cooked, whatever had focus */
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
    /* Power actions (clients already torn down above): shut down / reboot are
     * noreturn on success; fall through to the session exit if they fail. */
    if (power_action==PWR_SHUTDOWN) sys_shutdown();
    if (power_action==PWR_REBOOT)   sys_reboot();
    if (exit_to_shell) sys_gui_close();   /* "Log out to shell" -> CLI shell   */
    else               sys_logout();      /* "Log out (graphical)" -> login    */
    return 0;
}
