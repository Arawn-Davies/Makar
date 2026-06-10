/*
 * mxsettings.elf -- the centralised desktop Settings app, as a makx client.
 *
 * macOS-System-Settings layout: a left sidebar of categories + a right content
 * pane of grouped rows.  Each panel reimplements its controls inline and drives
 * the underlying mechanism directly -- the ~/.mxrc keys (mxrc.h), SYS_SETMODE,
 * the net syscalls -- so Settings is the one place to change everything (the
 * standalone mxdisplay/mxnet are subsumed over time).  No image decoders: the
 * wallpaper just sets ~/.mxrc Wallpaper= and the WM's poll applies it live.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"
#include "gui_browser.h"
#include "mxrc.h"
#include "time.h"

#define RGB GFX_RGB
#define COL_BG    UI_COL_FIELD                 /* content background          */
#define COL_SIDE  GFX_RGB(0x1b,0x22,0x2e)      /* sidebar background          */
#define COL_TEXT  UI_COL_TEXT
#define COL_MUTE  UI_COL_MUTED
#define COL_HDR   GFX_RGB(0x8a,0xc0,0xff)       /* category title             */
#define COL_OK    GFX_RGB(0x8a,0xe2,0x34)
#define COL_ERR   GFX_RGB(0xff,0x70,0x70)
#define SIDEBAR_W 160

/* ---- small freestanding helpers ----------------------------------------- */
static int  slen(const char *s){int n=0;while(s[n])n++;return n;}
static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static int  streq(const char *a,const char *b){int i=0;while(a[i]&&a[i]==b[i])i++;return a[i]==b[i];}
static char *u2s(unsigned v,char *o){char t[12];int i=0;if(!v)t[i++]='0';while(v){t[i++]=(char)('0'+v%10u);v/=10u;}int j=0;while(i)o[j++]=t[--i];o[j]=0;return o;}
/* zero-padded 2-digit into d[0..1] */
static void p2(char *d,int v){ d[0]=(char)('0'+(v/10)%10); d[1]=(char)('0'+v%10); }

enum { CAT_APPEARANCE, CAT_DISPLAY, CAT_STATUSBAR, CAT_NETWORK,
       CAT_DATETIME, CAT_AUTOSTART, CAT_N };
static const char *const CATS[CAT_N] = {
    "Appearance", "Display", "Status Bar", "Network", "Date & Time", "Autostart",
};

static mx_conn *g_c;            /* the connection (panels send WM messages)  */
static unsigned g_now;          /* sys_uptime() this frame (for countdowns)  */

static int section(gfx_surface *s, int x, int y, const char *title)
{ gfx_str(s, x, y, title, COL_MUTE); return y + 18; }

/* ---- Appearance: desktop wallpaper -------------------------------------- */
static char    ap_path[160];
static char    ap_msg[40];
static int     ap_init=0;
static browser ap_brz;          /* file picker for the Browse... button */
static void panel_appearance(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)ch;
    if (!ap_init){ if (mxrc_get("Wallpaper", ap_path, sizeof ap_path)!=0) ap_path[0]=0; ap_init=1; }
    int y = section(s, cx, cy, "WALLPAPER");
    gfx_str(s, cx, y, "Image path (PNG / BMP / JPEG):", COL_TEXT); y+=20;
    ui_textbox(u, s, cx, y, cw-208, 24, ap_path, sizeof ap_path);
    if (ui_button(u, s, cx+cw-198, y, 94, 24, "Browse...")){
        if (!ap_brz.cwd[0]) scpy(ap_brz.cwd, "/usr/share", sizeof ap_brz.cwd);
        char chosen[256]; chosen[0]=0;
        if (br_dialog_window(g_c, &ap_brz, 1, (char*)0, 0, chosen, sizeof chosen)==1 && chosen[0])
            scpy(ap_path, chosen, sizeof ap_path);
    }
    if (ui_button(u, s, cx+cw-100, y, 92, 24, "Apply")){
        mxrc_set("Wallpaper", ap_path);        /* the WM polls this key + repaints */
        scpy(ap_msg, ap_path[0] ? "Wallpaper set." : "Wallpaper cleared.", sizeof ap_msg);
    }
    y+=32;
    if (ap_msg[0]) { gfx_str(s, cx, y, ap_msg, COL_OK); y+=18; }
    gfx_str(s, cx, y+8, "Tip: also set from the image viewer (Set as wallpaper).", COL_MUTE);

    y = section(s, cx, y+40, "COLOUR SCHEME");
    gfx_str(s, cx, y, "Light / dark themes - coming soon.", COL_MUTE);
}

/* ---- Display: resolution + confirm/auto-revert -------------------------- */
typedef struct { const char *name; } dmode;
static const dmode MODES[] = {
    {"1920x1080"},{"1280x720"},{"1024x768"},{"800x600"},{"640x480"},
};
#define NMODES ((int)(sizeof(MODES)/sizeof(MODES[0])))
static char     d_cur[16], d_prev[16], d_msg[48];
static int      d_confirm=0, d_init=0;
static unsigned d_deadline=0;
static void d_refresh_cur(void){
    unsigned info=sys_fb_info(); unsigned w=(info>>16)&0xFFFF, h=info&0xFFFF;
    char num[12]; int n=0; u2s(w,num); for(int i=0;num[i];i++)d_cur[n++]=num[i];
    d_cur[n++]='x'; u2s(h,num); for(int i=0;num[i];i++)d_cur[n++]=num[i]; d_cur[n]=0;
}
static void panel_display(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)cw;(void)ch;
    if(!d_init){ d_refresh_cur(); d_init=1; }
    if (d_confirm && (int)(g_now - d_deadline) >= 0){     /* auto-revert */
        sys_setmode(d_prev); scpy(d_cur,d_prev,sizeof d_cur);
        scpy(d_msg,"Reverted (no confirmation).",sizeof d_msg); d_confirm=0;
    }
    int y = section(s, cx, cy, "RESOLUTION");
    if (!d_confirm){
        for (int i=0;i<NMODES;i++){
            int by=y+i*34;
            if (ui_button(u,s,cx,by,180,28,MODES[i].name)){
                scpy(d_prev,d_cur,sizeof d_prev);
                if (sys_setmode(MODES[i].name)==0){
                    scpy(d_cur,MODES[i].name,sizeof d_cur);
                    d_deadline=g_now+1500u; d_msg[0]=0; d_confirm=1;   /* 15s @100Hz */
                } else scpy(d_msg,"Mode not supported by this adapter.",sizeof d_msg);
            }
            if (streq(d_cur,MODES[i].name)) gfx_str(s,cx+192,by+10,"(current)",COL_OK);
        }
        if (d_msg[0]) gfx_str(s,cx,y+NMODES*34+8,d_msg,COL_ERR);
    } else {
        int left=((int)(d_deadline-g_now))/100; if(left<0)left=0;
        gfx_str(s,cx,y,"Keep this resolution?",COL_TEXT);
        char line[40]; scpy(line,"Reverting in ",sizeof line); int L=slen(line);
        char nb[12]; scpy(line+L,u2s((unsigned)left,nb),(int)sizeof line-L); L=slen(line);
        scpy(line+L,"s ...",(int)sizeof line-L);
        gfx_str(s,cx,y+20,line,COL_MUTE);
        if (ui_button(u,s,cx,y+48,150,30,"Keep changes")){ scpy(d_msg,"Resolution kept.",sizeof d_msg); d_confirm=0; }
        if (ui_button(u,s,cx+160,y+48,150,30,"Revert now")){ sys_setmode(d_prev); scpy(d_cur,d_prev,sizeof d_cur); scpy(d_msg,"Reverted.",sizeof d_msg); d_confirm=0; }
    }
}

/* ---- Status Bar: dock tray widget visibility ---------------------------- */
#define N_TRAY 6
static const struct { const char *key, *label; } TRAY[N_TRAY] = {
    {"TrayClock","Clock"},{"TrayDate","Date"},{"TrayNet","Network"},
    {"TrayCpu","CPU"},{"TrayRam","RAM"},{"TrayGpu","GPU"},
};
static int  t_val[N_TRAY], t_init=0;
static void panel_statusbar(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)cw;(void)ch;
    if(!t_init){ for(int i=0;i<N_TRAY;i++) t_val[i]=mxrc_get_int(TRAY[i].key,1); t_init=1; }
    int y = section(s, cx, cy, "SHOW IN THE STATUS BAR");
    for (int i=0;i<N_TRAY;i++){
        int ry=y+i*30;
        gfx_str(s,cx,ry+4,TRAY[i].label,COL_TEXT);
        if (ui_toggle(u,s,cx+200,ry,&t_val[i])){
            mxrc_set_int(TRAY[i].key, t_val[i]);
            mx_reload_prefs(g_c);              /* dock re-reads + repaints live */
        }
    }
}

/* ---- Network: status + DHCP + manual/static ----------------------------- */
static char n_info[512]; static int n_init=0;
static char n_ip[20], n_mask[20]="255.255.255.0", n_gw[20], n_dns[20], n_msg[40];
static int  n_static=0;
static void n_refresh(void){ int n=sys_net_info(n_info,(unsigned)sizeof n_info-1); if(n<0)n=0; n_info[n]=0; }
static void parse_ip(const char *s, unsigned char o[4]){
    o[0]=o[1]=o[2]=o[3]=0; int part=0,val=0,have=0;
    for(;;s++){ char ch=*s;
        if(ch>='0'&&ch<='9'){ val=val*10+(ch-'0'); have=1; }
        else { if(have&&part<4) o[part++]=(unsigned char)val; val=0; have=0; if(ch=='\0')break; }
    }
}
static void panel_network(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)ch;
    if(!n_init){ n_refresh(); n_init=1; }
    int y = section(s, cx, cy, "CONNECTION (DHCP)");
    int bx=cx;
    if (ui_button(u,s,bx,y,80,24,"Renew"))   { sys_net_ctl(NET_CTL_DHCP_RENEW);   n_refresh(); } bx+=86;
    if (ui_button(u,s,bx,y,80,24,"Release")) { sys_net_ctl(NET_CTL_DHCP_RELEASE); n_refresh(); } bx+=86;
    if (ui_button(u,s,bx,y,92,24,"Flush DNS")){ sys_net_ctl(NET_CTL_DNS_FLUSH);   n_refresh(); }
    y+=32;
    for (const char *p=n_info; *p && y<cy+150; ){
        char line[96]; int i=0; while(*p && *p!='\n' && i<(int)sizeof line-1) line[i++]=*p++;
        line[i]=0; if(*p=='\n')p++;
        gfx_str_clip(s,cx,y,line,COL_TEXT,cx+cw); y+=12;
    }

    y = section(s, cx, y+12, "MANUAL CONFIGURATION");
    gfx_str(s,cx,y+4,"Use a static address", COL_TEXT);
    ui_toggle(u,s,cx+200,y,&n_static);
    y+=30;
    if (n_static){
        struct { const char *lab; char *buf; } f[4] = {
            {"IP",n_ip},{"Netmask",n_mask},{"Gateway",n_gw},{"DNS",n_dns} };
        for (int i=0;i<4;i++){
            gfx_str(s,cx,y+5,f[i].lab,COL_MUTE);
            ui_textbox(u,s,cx+80,y,150,22,f[i].buf,20);
            y+=28;
        }
        if (ui_button(u,s,cx,y+4,140,28,"Apply static")){
            net_cfg_t cfg; cfg.dhcp=0;
            parse_ip(n_ip,cfg.ip); parse_ip(n_mask,cfg.mask);
            parse_ip(n_gw,cfg.gw); parse_ip(n_dns,cfg.dns);
            scpy(n_msg, sys_net_config(&cfg)==0 ? "Static config applied." : "Failed.", sizeof n_msg);
            n_refresh();
        }
    } else {
        if (ui_button(u,s,cx,y+4,140,28,"Switch to DHCP")){
            net_cfg_t cfg; for(unsigned i=0;i<sizeof cfg;i++)((unsigned char*)&cfg)[i]=0; cfg.dhcp=1;
            scpy(n_msg, sys_net_config(&cfg)==0 ? "DHCP requested." : "Failed.", sizeof n_msg);
            n_refresh();
        }
    }
    if (n_msg[0]) gfx_str(s,cx+150,y+10,n_msg, n_msg[0]=='F'?COL_ERR:COL_OK);
}

/* ---- Date & Time: live clock + set (SYS_SETTIME) ------------------------ */
static int  dt_y, dt_mo, dt_d, dt_h, dt_mi, dt_s;
static char dt_msg[40];
static int  dt_init=0;
/* centre a label under a spinner's *value field* (the box left of the +/-
 * steppers, width w-UI_SPIN_BTN) so the YYYY/MM/DD row lines up under the digits */
static void splbl(gfx_surface *s,int x,int w,int y,const char *t){
    int vw=w-UI_SPIN_BTN; int tw=gfx_text_w(t); gfx_str(s,x+(vw-tw)/2,y,t,COL_MUTE);
}
static void panel_datetime(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)cw;(void)ch;
    int y = section(s, cx, cy, "CURRENT (UTC)");
    struct timeval tv; struct tm tmv; int have=0;
    if (sys_gettimeofday(&tv)==0){ time_t t=(time_t)tv.tv_sec; gmtime_r(&t,&tmv); have=1; }
    if (have){
        char buf[24], nb[12]; int n=0;
        u2s((unsigned)(tmv.tm_year+1900),nb); for(int i=0;nb[i];i++)buf[n++]=nb[i]; buf[n++]='-';
        p2(buf+n,tmv.tm_mon+1);n+=2;buf[n++]='-';p2(buf+n,tmv.tm_mday);n+=2;buf[n++]=' ';
        p2(buf+n,tmv.tm_hour);n+=2;buf[n++]=':';p2(buf+n,tmv.tm_min);n+=2;buf[n++]=':';p2(buf+n,tmv.tm_sec);n+=2;buf[n]=0;
        gfx_str(s,cx,y,buf,COL_TEXT);
        if(!dt_init){                          /* prefill the spin fields once */
            dt_y=tmv.tm_year+1900; dt_mo=tmv.tm_mon+1; dt_d=tmv.tm_mday;
            dt_h=tmv.tm_hour; dt_mi=tmv.tm_min; dt_s=tmv.tm_sec; dt_init=1;
        }
    } else gfx_str(s,cx,y,"(clock unavailable)",COL_ERR);

    y = section(s, cx, y+34, "SET CLOCK (UTC)");
    int yw=62, mw=46, g=6;                 /* fixed spinner widths + gap */
    int xY  = cx+40;
    int xMo = xY  + yw + g;
    int xD  = xMo + mw + g;
    int xH  = xD  + mw + 44;               /* gap for the "Time" label */
    int xMi = xH  + mw + g;
    int xS  = xMi + mw + g;
    gfx_str(s,cx,        y+(22-8)/2,"Date",COL_MUTE);
    gfx_str(s,xD+mw+8,   y+(22-8)/2,"Time",COL_MUTE);
    ui_spinner(u,s,xY ,y,yw,22,&dt_y ,1970,2099);
    ui_spinner(u,s,xMo,y,mw,22,&dt_mo,1,12);
    ui_spinner(u,s,xD ,y,mw,22,&dt_d ,1,31);
    ui_spinner(u,s,xH ,y,mw,22,&dt_h ,0,23);
    ui_spinner(u,s,xMi,y,mw,22,&dt_mi,0,59);
    ui_spinner(u,s,xS ,y,mw,22,&dt_s ,0,59);
    splbl(s,xY ,yw,y+26,"YYYY"); splbl(s,xMo,mw,y+26,"MM"); splbl(s,xD,mw,y+26,"DD");
    splbl(s,xH ,mw,y+26,"HH");   splbl(s,xMi,mw,y+26,"MM"); splbl(s,xS,mw,y+26,"SS");
    if (ui_button(u,s,cx,y+48,120,28,"Set clock")){
        int rc=sys_settime(dt_y,dt_mo,dt_d,dt_h,dt_mi,dt_s);
        scpy(dt_msg, rc==0 ? "Clock updated." : "Invalid date/time.", sizeof dt_msg);
    }
    if (dt_msg[0]) gfx_str(s,cx+132,y+54,dt_msg, dt_msg[0]=='C'?COL_OK:COL_ERR);
}

/* ---- Autostart: apps launched at login (~/.mxrc Autostart=) -------------- */
#define N_AUTO 9
static const struct { const char *path, *name; } AUTOAPPS[N_AUTO] = {
    {"/apps/mxterm.elf","Terminal"},{"/apps/mxfiles.elf","Files"},
    {"/apps/mxedit.elf","Editor"},{"/apps/mxclock.elf","Clock"},
    {"/apps/mxcalc.elf","Calculator"},{"/apps/mxnet.elf","Network"},
    {"/apps/mximg.elf","Image Viewer"},{"/apps/mxweb.elf","Web Browser"},
    {"/apps/mxtasks.elf","Task Manager"},
};
static int  a_val[N_AUTO], a_init=0;
static int  a_in_list(const char *list, const char *path){
    int pl=slen(path);
    for (const char *p=list; *p; ){
        const char *q=p; int n=0; while(q[n] && q[n]!=',') n++;
        if (n==pl){ int i=0; while(i<pl && p[i]==path[i]) i++; if(i==pl) return 1; }
        p+=n; while(*p==',') p++;
    }
    return 0;
}
static void a_rebuild(void){            /* write the list from the toggles */
    char list[512]; int o=0;
    for (int i=0;i<N_AUTO;i++) if (a_val[i]){
        if (o && o<(int)sizeof list-1) list[o++]=',';
        for (const char *p=AUTOAPPS[i].path; *p && o<(int)sizeof list-1; ) list[o++]=*p++;
    }
    list[o]=0; mxrc_set("Autostart", list);
}
static void panel_autostart(ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    (void)cw;(void)ch;
    if(!a_init){ char list[512]; if(mxrc_get("Autostart",list,sizeof list)!=0)list[0]=0;
                 for(int i=0;i<N_AUTO;i++) a_val[i]=a_in_list(list,AUTOAPPS[i].path); a_init=1; }
    int y = section(s, cx, cy, "LAUNCH AT LOGIN");
    for (int i=0;i<N_AUTO;i++){
        int ry=y+i*26;
        gfx_str(s,cx,ry+4,AUTOAPPS[i].name,COL_TEXT);
        if (ui_toggle(u,s,cx+200,ry,&a_val[i])) a_rebuild();
    }
}

static void draw_panel(int cat, ui_ctx *u, gfx_surface *s, int cx, int cy, int cw, int ch)
{
    switch (cat) {
    case CAT_APPEARANCE: panel_appearance(u,s,cx,cy,cw,ch); break;
    case CAT_DISPLAY:    panel_display   (u,s,cx,cy,cw,ch); break;
    case CAT_STATUSBAR:  panel_statusbar (u,s,cx,cy,cw,ch); break;
    case CAT_NETWORK:    panel_network   (u,s,cx,cy,cw,ch); break;
    case CAT_DATETIME:   panel_datetime  (u,s,cx,cy,cw,ch); break;
    case CAT_AUTOSTART:  panel_autostart (u,s,cx,cy,cw,ch); break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 720, 520, MX_F_RESIZABLE) != 0) return 1;
    g_c = &c;
    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;

    int sel=0, scroll=0, g_menu=-1, g_about=0, g_quit=0;
    int first=1, lmx=-1, lmy=-1, lkey=-2; unsigned last=0;

    while (!c.closed && !g_quit) {
        mx_pump(&c);
        int key=-1, k; while ((k=mx_key(&c))>=0) key=k;
        g_now = sys_uptime();
        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        int kch=(key!=lkey); lkey=key;
        int tick=(g_now-last)>=25u;        /* ~4 Hz: drive the clock + countdown */
        if(!(first||tick||c.mpressed||c.mreleased||c.rpressed||moved||kch||c.resized)){ sys_yield(); continue; }
        last=g_now; first=0;

        gfx_surface *s=&c.surf; int top=UI_MENUBAR_H;
        gfx_fill(s,0,top,SIDEBAR_W,s->h-top,COL_SIDE);
        gfx_fill(s,SIDEBAR_W,top,s->w-SIDEBAR_W,s->h-top,COL_BG);

        int busy=(g_menu>=0)||g_about;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,busy?-1:key);
        ui_gate g; ui_gate_begin(&u,&g,busy);

        ui_listbox(&u,s,6,top+8,SIDEBAR_W-12,s->h-top-16,CATS,CAT_N,&sel,&scroll);

        int cx=SIDEBAR_W+18, cy=top+14, cw=s->w-SIDEBAR_W-36;
        gfx_str(s,cx,cy,CATS[sel],COL_HDR);
        draw_panel(sel,&u,s,cx,cy+26,cw,s->h-(cy+26)-10);

        ui_gate_end(&u,&g);
        static const char *al[]={"Makar Settings (mxsettings)","(c) 2026 Arawn Davies  --  MIT","","Centralised desktop settings: appearance, display, status bar,","network, date & time, autostart.","Part of Makar OS."};
        ui_appbar(&u,s,"mxsettings",al,6,0,0,&g_menu,&g_about,&g_quit);
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
