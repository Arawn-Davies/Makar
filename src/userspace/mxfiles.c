/*
 * mxfiles.elf -- file browser, as a makx client.  Navigates with the shared
 * gui_browser model and offers two views, like a desktop file manager:
 *   - List view: name + size + modified date (rows, via ui_listbox).
 *   - Icon view: a grid of folder/file glyphs with names underneath.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "makx.h"

#define RGB GFX_RGB
static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}

static browser g_files;
static char    pathbox[256];
static int     g_view = 0;            /* 0 = list, 1 = icons */
static mx_conn *g_conn;               /* for mx_open (default-app dispatch) */

/* per-entry list-view rows ("name   size   date"), rebuilt on navigation */
static char        g_row[BR_MAX][80];
static const char *g_rowptr[BR_MAX];
static int         g_rows_for = -1;   /* g_loadid the rows were built for */
static int         g_loadid = 0;      /* bumped on every (re)load */

/* menus (T52): top bar + About modal + right-click context menu */
static int g_menu=-1, g_about=0, g_ctx=0, g_ctx_x, g_ctx_y, g_exit_req=0;
enum { A_OPEN=1, A_REFRESH, A_UP, A_EXIT, A_VLIST, A_VICON, A_ABOUT };

/* append helpers (freestanding) */
static char *pcat(char *p, const char *s){ while(*s) *p++=*s++; return p; }
static char *pnum(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *ppad(char *p, char *base, int col){ while(p-base < col) *p++=' '; return p; }

static char *fmt_size(char *p, unsigned sz){
    if (sz < 1024u)            { p=pnum(p,sz); *p++='B'; }
    else if (sz < 1024u*1024u) { p=pnum(p,(sz+512u)/1024u); *p++='K'; }
    else                       { p=pnum(p,(sz+512u*1024u)/(1024u*1024u)); *p++='M'; }
    return p;
}
/* unix epoch (s) -> "YYYY-MM-DD HH:MM" (civil-from-days, UTC) */
static char *fmt_date(char *p, unsigned t){
    long days=(long)(t/86400u); int secs=(int)(t%86400u);
    long z=days+719468; long era=(z>=0?z:z-146096)/146097;
    unsigned doe=(unsigned)(z-era*146097);
    unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    long y=(long)yoe+era*400;
    unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    unsigned mp=(5*doy+2)/153, d=doy-(153*mp+2)/5+1, m=mp<10?mp+3:mp-9;
    if(m<=2) y++;
    int hh=secs/3600, mm=(secs/60)%60;
    char b[6]; int i=0; { long yy=y; char t[6]; int k=0; while(yy){t[k++]=(char)('0'+yy%10);yy/=10;} while(k<4)t[k++]='0'; while(k)b[i++]=t[--k]; } b[i]=0;
    p=pcat(p,b); *p++='-';
    *p++=(char)('0'+m/10); *p++=(char)('0'+m%10); *p++='-';
    *p++=(char)('0'+d/10); *p++=(char)('0'+d%10); *p++=' ';
    *p++=(char)('0'+hh/10);*p++=(char)('0'+hh%10);*p++=':';
    *p++=(char)('0'+mm/10);*p++=(char)('0'+mm%10);
    return p;
}

/* stat each entry and build the list-view rows for the current directory */
static void build_rows(void){
    if (g_rows_for == g_loadid) return;
    g_rows_for = g_loadid;
    for (int i=0;i<g_files.n;i++){
        char *p=g_row[i], *base=g_row[i];
        const char *nm=g_files.name[i];
        for (const char *q=nm; *q && p-base<42; q++) *p++=*q;     /* name (keeps trailing '/') */
        p=ppad(p,base,44);
        if (g_files.type[i]==DT_DIR){ p=pcat(p,"<dir>"); }
        else {
            char path[320]; int k=0;
            for (const char *q=g_files.cwd; *q && k<300; q++) path[k++]=*q;
            if (k && path[k-1]!='/') path[k++]='/';
            for (const char *q=nm; *q && k<318; q++){ if(*q=='/'&&q[1]==0) break; path[k++]=*q; }
            path[k]=0;
            struct stat st;
            if (sys_stat(path,&st)==0){ p=fmt_size(p,st.st_size); p=ppad(p,base,54); p=fmt_date(p,st.st_mtime); }
            else p=pcat(p,"-");
        }
        *p=0; g_rowptr[i]=g_row[i];
    }
}

static void open_sel(void){
    if(g_files.sel<0||g_files.sel>=g_files.n) return;
    if(br_sel_isdir(&g_files)){ br_enter_sel(&g_files); g_loadid++; return; }
    /* a regular file: hand the WM the full path -> it opens the default app. */
    const char *nm=g_files.name[g_files.sel];
    char path[320]; int k=0;
    for(const char *q=g_files.cwd; *q && k<300; q++) path[k++]=*q;
    if(k && path[k-1]!='/') path[k++]='/';
    for(const char *q=nm; *q && k<318; q++){ if(*q=='/'&&q[1]==0) break; path[k++]=*q; }
    path[k]=0;
    if(g_conn) mx_open(g_conn, path);
}

static void files_do(int a){
    switch(a){
    case A_OPEN:    open_sel(); break;
    case A_REFRESH: br_load(&g_files); g_loadid++; break;
    case A_UP:      br_up(&g_files);   g_loadid++; break;
    case A_EXIT:    g_exit_req=1; break;
    case A_VLIST:   g_view=0; g_files.scroll=0; break;
    case A_VICON:   g_view=1; g_files.scroll=0; break;
    case A_ABOUT:   g_about=1; break;
    }
}

/* ---- icon-grid view ---- */
static void draw_folder(gfx_surface*s,int x,int y){
    gfx_fill(s,x+2,y+2,12,5,RGB(0xc8,0x8a,0x20));
    gfx_round(s,x,y+6,30,18,RGB(0xf0,0xa8,0x30),UI_COL_FIELD);
}
static void draw_file(gfx_surface*s,int x,int y){
    gfx_fill(s,x+5,y,20,24,RGB(0xe6,0xea,0xf0));
    gfx_fill(s,x+19,y,6,6,UI_COL_FIELD);                      /* folded corner */
    for(int k=0;k<3;k++) gfx_fill(s,x+8,y+7+k*5,13,2,RGB(0x90,0xa0,0xb5));
    gfx_outline(s,x+5,y,20,24,RGB(0x6a,0x9a,0xc0));
}
/* grid: cells cw x ch; vertical scroll by g_files.scroll (top row).  Returns 1
 * if a cell was activated (click on the already-selected item) -> caller opens. */
static int icon_view(ui_ctx*u, gfx_surface*s, int x,int y,int w,int h){
    int cw=104, ch=78, pad=6, cols=w/cw; if(cols<1)cols=1;
    int rows=(g_files.n+cols-1)/cols, visrows=h/ch; if(visrows<1)visrows=1;
    if(g_files.scroll > rows-visrows) g_files.scroll = rows-visrows>0?rows-visrows:0;
    if(g_files.scroll < 0) g_files.scroll=0;
    int act=0;
    for(int i=0;i<g_files.n;i++){
        int r=i/cols - g_files.scroll, col=i%cols;
        if(r<0||r>=visrows) continue;
        int cx=x+col*cw, cy=y+r*ch;
        int hot = u->mx>=cx&&u->mx<cx+cw&&u->my>=cy&&u->my<cy+ch;
        if(i==g_files.sel) gfx_round(s,cx+pad,cy+pad,cw-2*pad,ch-pad,UI_COL_BTN_ACT,UI_COL_FIELD);
        else if(hot)       gfx_round(s,cx+pad,cy+pad,cw-2*pad,ch-pad,UI_COL_BTN_HOT,UI_COL_FIELD);
        int gx=cx+(cw-30)/2, gy=cy+12;
        if(g_files.type[i]==DT_DIR) draw_folder(s,gx,gy); else draw_file(s,gx,gy);
        /* label: centre short names, left-clip long ones to the padded width */
        char nm[18]; scpy(nm,g_files.name[i],sizeof nm);
        int tw=gfx_text_w(nm), avail=cw-2*pad, lx=cx+(cw-tw)/2;
        if(tw>avail) lx=cx+pad;
        gfx_str_clip(s,lx,cy+ch-18,nm,UI_COL_TEXT,cx+cw-pad);
        if(hot && u->mpressed){ if(i==g_files.sel) act=1; g_files.sel=i; u->got_input=1; }
    }
    return act;
}

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,560,380,MX_F_RESIZABLE)!=0) return 1;
    g_conn=&c;
    scpy(g_files.cwd,"/",sizeof g_files.cwd); br_load(&g_files); g_loadid++;

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1, lmx=-1, lmy=-1, lfocus=-1;

    while(!c.closed && !g_exit_req){
        mx_pump(&c);
        int key=mx_key(&c);
        int changed = first || key>=0 || c.mpressed || c.mreleased || c.rpressed ||
                      c.mx!=lmx || c.my!=lmy || c.focused!=lfocus||c.resized;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; first=0;
        if(!changed){ sys_yield(); continue; }

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,UI_COL_FIELD);

        int busy = (g_menu>=0) || g_about || g_ctx;
        int kin  = (key=='\t') ? -1 : key;
        int rp   = busy ? 0 : c.rpressed;
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,kin);
        /* while an overlay (menu/About/context) is up, gate the content widgets;
         * the overlay itself gets the real input restored below. */
        int rk=u.key, rpr=u.mpressed, rdn=u.mdown, rrl=u.mreleased;
        if(busy){ u.key=-1; u.mpressed=u.mdown=u.mreleased=0; }

        int top=UI_MENUBAR_H;
        int up_c=ui_button(&u,s,6,top+6,46,20,"Up");
        int op_c=ui_button(&u,s,56,top+6,52,20,"Open");
        int rf_c=ui_button(&u,s,112,top+6,62,20,"Refresh");
        int vw_c=ui_button(&u,s,178,top+6,86,20, g_view? "View: Icons":"View: List");
        int pbx=270, pbw=s->w-12-pbx-44; if(pbw<50)pbw=50;
        ui_textbox(&u,s,pbx,top+6,pbw,20,pathbox,(int)sizeof pathbox);
        int go_c=ui_button(&u,s,pbx+pbw+4,top+6,40,20,"Go");

        if(up_c){ br_up(&g_files); g_loadid++; }
        if(rf_c){ br_load(&g_files); g_loadid++; }
        if(vw_c){ g_view=!g_view; g_files.scroll=0; }
        if(go_c&&pathbox[0]){ scpy(g_files.cwd,pathbox,sizeof g_files.cwd); g_files.sel=g_files.scroll=0; br_load(&g_files); g_loadid++; }

        ui_label(&u,s,6,top+30,g_files.cwd,UI_COL_MUTED);

        int sbw=12;
        int lx=6, ly=top+50, lw=s->w-12-sbw, lh=s->h-(top+56), prev=g_files.sel;
        int rclick = rp && c.mx>=lx && c.mx<lx+lw && c.my>=ly && c.my<ly+lh;
        if(g_view){
            if(!busy && kin==0x81) g_files.scroll++;                  /* arrow down */
            else if(!busy && kin==0x80 && g_files.scroll>0) g_files.scroll--;  /* up */
            if(icon_view(&u,s,lx,ly,lw,lh)) open_sel();
            int cw=104, cols=lw/cw; if(cols<1)cols=1;
            int rows=(g_files.n+cols-1)/cols, visr=lh/78; if(visr<1)visr=1;
            ui_vscroll(&u,s,lx+lw,ly,sbw,lh,rows,visr,&g_files.scroll);
        } else {
            build_rows();
            ui_listbox(&u,s,lx,ly,lw,lh,g_rowptr,g_files.n,&g_files.sel,&g_files.scroll);
            int visr=lh/12; if(visr<1)visr=1;
            ui_vscroll(&u,s,lx+lw,ly,sbw,lh,g_files.n,visr,&g_files.scroll);
            if(rclick){ int row=g_files.scroll+(c.my-ly)/12; if(row>=0&&row<g_files.n)g_files.sel=row; }
            if(c.focused && !busy && c.mpressed && prev==g_files.sel &&
               c.mx>=lx&&c.mx<lx+lw&&c.my>=ly&&c.my<ly+lh) open_sel();
        }
        if(op_c) open_sel();
        else if(c.focused && !busy && kin=='\n') open_sel();
        if(rclick){ g_ctx=1; g_ctx_x=c.mx; g_ctx_y=c.my; }

        /* restore the real input snapshot for the overlay menus */
        u.key=rk; u.mpressed=rpr; u.mdown=rdn; u.mreleased=rrl;

        /* menu bar + About + context menu (overlays -- draw last) */
        static const ui_menu_item fitems[]={{"Open",A_OPEN,1,0},{"Refresh",A_REFRESH,1,0},{"Up one level",A_UP,1,0},{0,0,0,0},{"Exit",A_EXIT,1,0}};
        static const ui_menu_item vitems[]={{"List view",A_VLIST,1,0},{"Icon view",A_VICON,1,0}};
        static const ui_menu_item hitems[]={{"About mxfiles",A_ABOUT,1,0}};
        ui_menu menus[]={{"File",fitems,5},{"View",vitems,2},{"Help",hitems,1}};
        int act=ui_menubar(&u,s,menus,3,&g_menu);
        if(act) files_do(act);

        static const char *al[]={"Makar file browser (mxfiles)","(c) 2026 Arawn Davies  --  MIT","","List / icon views; opens files in the default app.","Part of Makar OS."};
        ui_about(&u,s,"About mxfiles",al,5,&g_about);

        int sel_ok = g_files.sel>=0 && g_files.sel<g_files.n;
        ui_menu_item citems[]={{"Open",A_OPEN,sel_ok,0},{"Refresh",A_REFRESH,1,0}};
        int cact=ui_context_menu(&u,s,g_ctx_x,g_ctx_y,citems,2,&g_ctx);
        if(cact) files_do(cact);

        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
