/*
 * mxterm.elf -- a real ANSI/VT100+ terminal, as a makx client.  Forks
 * /apps/sh.elf over a pair of pipes, feeds the child's byte stream through the
 * vt100 emulator core (vt100.c), and renders its colour cell grid into the makx
 * surface; forwards keys the display server delivers to the shell's stdin.
 *
 * Was a single-colour glass-TTY; now parses CSI/SGR/scroll-region/alt-screen so
 * TUI programs (maktop, vix, ls --color, ...) render correctly in the window,
 * and (T52) carries a Windows-style menu bar + right-click menu, drag-to-select
 * text -> system clipboard, and a scrollback buffer with a draggable scrollbar.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"
#include "vt100.h"

#define RGB GFX_RGB
#define TMENU_H 18              /* top menu-bar strip (= UI_MENUBAR_H)          */
#define SBW     12              /* scrollbar strip on the right                 */
#define COL_SEL RGB(0x33,0x55,0x88)

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static char *scat(char *d,const char *s){while(*d)d++;while(*s)*d++=*s++;*d=0;return d;}

/* Standard 16-colour ANSI palette (0-7 normal, 8-15 bright). */
static const gfx_u32 PAL[16] = {
    RGB(0x00,0x00,0x00), RGB(0xaa,0x00,0x00), RGB(0x00,0xaa,0x00), RGB(0xaa,0x55,0x00),
    RGB(0x00,0x00,0xaa), RGB(0xaa,0x00,0xaa), RGB(0x00,0xaa,0xaa), RGB(0xc0,0xc4,0xbe),
    RGB(0x55,0x55,0x55), RGB(0xff,0x55,0x55), RGB(0x55,0xff,0x55), RGB(0xff,0xff,0x55),
    RGB(0x55,0x55,0xff), RGB(0xff,0x55,0xff), RGB(0x55,0xff,0xff), RGB(0xff,0xff,0xff) };

static vt_term vt;
static int term_pid=-1, term_in=-1, term_out=-1;
static const char *g_runcmd = 0;

/* ---- menus / selection / scrollback view (T52) -------------------------- */
static int g_menu=-1, g_about=0, g_ctx=0, g_ctx_x, g_ctx_y, g_exit_req=0;
static int sb_view=0;           /* lines scrolled up from the bottom (0 = live) */
/* selection in (logical line, col); logical line spans [0, sb_count+rows):
 * < sb_count is scrollback, else live row (line - sb_count). */
static int sel_on=0, sel_a_ln, sel_a_col, sel_c_ln, sel_c_col;
enum { T_COPY=1, T_PASTE, T_SELALL, T_ABOUT };

static void feed(const char *s){ while(*s) vt_putc(&vt,(unsigned char)*s++); }

static void term_spawn(void){
    if(term_pid>0)return;
    int ip[2],op[2];
    if(sys_pipe(ip)<0||sys_pipe(op)<0){ feed("terminal: pipe failed\r\n"); return; }
    int pid=sys_fork();
    if(pid<0){ feed("terminal: fork failed\r\n"); return; }
    if(pid==0){
        sys_close(ip[1]);sys_close(op[0]);
        sys_dup2(ip[0],0);sys_dup2(op[1],1);sys_dup2(op[1],2);
        sys_close(ip[0]);sys_close(op[1]);
        if(g_runcmd){
            char *av[4]={"sh.elf","-c",(char*)g_runcmd,0};
            sys_execve("/apps/sh.elf",av,(char*const*)0);
            sys_exit(127);
        }
        char user[48],uarg[64];char*av[3]={"sh.elf",0,0};
        if(sys_whoami(user,sizeof user)>0){scpy(uarg,"--user=",sizeof uarg);scat(uarg,user);av[1]=uarg;}
        sys_execve("/apps/sh.elf",av,(char*const*)0);
        sys_exit(127);
    }
    term_pid=pid;term_in=ip[1];term_out=op[0];
    sys_close(ip[0]);sys_close(op[1]);
    sys_fcntl(term_out,F_SETFL,O_NONBLOCK);
    sys_fcntl(term_in,F_SETFL,O_NONBLOCK);
    sys_pty_winsize(term_out, vt.cols, vt.rows);
}
static void term_kill(void){
    if(term_pid>0){sys_kill(term_pid,SIGKILL);int st;sys_wait4(term_pid,&st,0);}
    if(term_in>=0)sys_close(term_in);
    if(term_out>=0)sys_close(term_out);
    term_pid=-1;term_in=-1;term_out=-1;
}
static int term_pump(void){
    if(term_out<0)return 0;
    unsigned char b[256];int dirty=0;
    for(;;){long n=sys_read(term_out,b,sizeof b);if(n<=0)break;for(long i=0;i<n;i++)vt_putc(&vt,b[i]);dirty=1;if(n<(long)sizeof b)break;}
    if(term_pid>0){int st;if(sys_wait4(term_pid,&st,WNOHANG)==term_pid){term_pid=-1;feed("\r\n[shell exited]\r\n");dirty=1;}}
    return dirty;
}
static void term_key(int k){
    if(term_in<0)return;
    unsigned char b=(unsigned char)k;
    if(b=='\r')b='\n';
    if(b==0x03 && term_pid>0) sys_kill(term_pid,SIGINT);
    sys_write(term_in,&b,1);
}
static void term_paste(void){
    if(term_in<0)return;
    char cb[4096]; int n=sys_clip_get(cb,sizeof cb); if(n<=0)return;
    if(n>(int)sizeof cb)n=(int)sizeof cb;
    for(int i=0;i<n;i++){ unsigned char b=(unsigned char)cb[i]; if(b=='\r')b='\n'; sys_write(term_in,&b,1); }
}

/* ---- scrollback addressing + selection ---------------------------------- */
/* Pointer to the VT_MAXCOLS cells of logical line `ln`. */
static vt_cell *log_row(int ln){
    if(ln < vt.sb_count){
        int idx = vt.sb_head - vt.sb_count + ln; idx %= VT_SCROLLBACK; if(idx<0) idx+=VT_SCROLLBACK;
        return vt.sb[idx];
    }
    return vt.cell[ln - vt.sb_count];
}
static int total_lines(void){ return vt.sb_count + vt.rows; }
static int has_sel(void){ return sel_on && !(sel_a_ln==sel_c_ln && sel_a_col==sel_c_col); }
static void sel_bounds(int *lln,int *lc,int *hln,int *hc){
    int aln=sel_a_ln,ac=sel_a_col,bln=sel_c_ln,bc=sel_c_col;
    if(aln>bln || (aln==bln && ac>bc)){ int t; t=aln;aln=bln;bln=t; t=ac;ac=bc;bc=t; }
    *lln=aln;*lc=ac;*hln=bln;*hc=bc;
}
static int cell_in_sel(int ln,int c,int lln,int lc,int hln,int hc){
    if(ln<lln||ln>hln) return 0;             /* inclusive of both endpoints */
    if(ln==lln && c<lc) return 0;
    if(ln==hln && c>hc) return 0;
    return 1;
}
static void term_copy(void){
    if(!has_sel()) return;
    int lln,lc,hln,hc; sel_bounds(&lln,&lc,&hln,&hc);
    static char buf[16384]; int o=0;
    for(int ln=lln; ln<=hln && o<(int)sizeof buf-1; ln++){
        vt_cell *row=log_row(ln);
        int c0=(ln==lln)?lc:0, c1=(ln==hln)?hc:vt.cols-1;
        if(c1>vt.cols-1)c1=vt.cols-1;
        int last=c0-1; for(int c=c0;c<=c1;c++) if(row[c].ch!=' ') last=c;   /* trim trailing space */
        for(int c=c0;c<=last && o<(int)sizeof buf-1;c++) buf[o++]=(char)row[c].ch;
        if(ln<hln && o<(int)sizeof buf-1) buf[o++]='\n';
    }
    if(o>0) sys_clip_set(buf,(unsigned)o);
}
static void term_selall(void){
    sel_on=1; sel_a_ln=0; sel_a_col=0;
    sel_c_ln=total_lines()-1; sel_c_col=vt.cols-1;
}
static void term_do(int a){
    if(a==T_COPY) term_copy();
    else if(a==T_PASTE){ term_paste(); sb_view=0; }
    else if(a==T_SELALL) term_selall();
    else if(a==T_ABOUT) g_about=1;
}

/* Grid geometry: 8x8 glyphs, 4px margin, below the menu bar, left of the bar. */
static void term_fit(gfx_surface *s){
    int cols=(s->w-8-SBW)/8, rows=(s->h-8-TMENU_H)/8;
    if(cols<1)cols=1; if(rows<1)rows=1;
    if(cols!=vt.cols||rows!=vt.rows){
        vt_resize(&vt,cols,rows);
        if(term_out>=0) sys_pty_winsize(term_out, vt.cols, vt.rows);
    }
}
/* Map a client pixel to a (logical line, col); clamped to the grid. */
static void pix_to_cell(int px,int py,int *ln,int *col){
    int c=(px-4)/8; if(c<0)c=0; if(c>vt.cols-1)c=vt.cols-1;
    int vr=(py-(TMENU_H+4))/8; if(vr<0)vr=0; if(vr>vt.rows-1)vr=vt.rows-1;
    int top = vt.sb_count - sb_view;
    *ln = top+vr; *col=c;
}
static int in_grid(int px,int py){
    return px>=4 && px<4+vt.cols*8 && py>=TMENU_H+4 && py<TMENU_H+4+vt.rows*8;
}
static void term_draw(gfx_surface *s, int focused){
    gfx_fill(s,0,0,s->w,s->h,PAL[0]);
    int top = vt.sb_count - sb_view;
    int lln=0,lc=0,hln=0,hc=0, hs=has_sel(); if(hs) sel_bounds(&lln,&lc,&hln,&hc);
    for(int vr=0; vr<vt.rows; vr++){
        int ln=top+vr;
        if(ln<0||ln>=total_lines()) continue;
        vt_cell *row=log_row(ln);
        for(int c=0;c<vt.cols;c++){
            vt_cell *cl=&row[c];
            int x=4+c*8, y=TMENU_H+4+vr*8;
            int seld = hs && cell_in_sel(ln,c,lln,lc,hln,hc);
            if(seld) gfx_fill(s,x,y,8,8,COL_SEL);
            else if(cl->bg) gfx_fill(s,x,y,8,8,PAL[cl->bg&15]);
            if(cl->ch!=' ') gfx_char(s,x,y,cl->ch,PAL[cl->fg&15]);
        }
    }
    /* cursor only when following the live tail */
    if(focused && sb_view==0 && vt.cursor_visible && vt.cx<vt.cols && vt.cy<vt.rows)
        gfx_fill(s,4+vt.cx*8,TMENU_H+4+vt.cy*8,8,8,RGB(0x8a,0xe2,0x34));
}

static int streq(const char *a,const char *b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }

int main(int argc,char**argv){
    mx_conn c;
    if(mx_connect(&c,argc,argv,640,400,MX_F_RESIZABLE)!=0) return 1;
    for(int i=1;i<argc;i++){
        if(streq(argv[i],"-makx")){ i++; continue; }
        if(argv[i][0]=='-') continue;
        g_runcmd=argv[i]; break;
    }
    { int cols=(c.surf.w-8-SBW)/8, rows=(c.surf.h-8-TMENU_H)/8;
      if(cols<1)cols=1; if(rows<1)rows=1; vt_init(&vt,cols,rows); }
    term_spawn();

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    int first=1, lmx=-1, lmy=-1, lfocus=-1;
    while(!c.closed && !g_exit_req){
        mx_pump(&c);
        if(c.resized) term_fit(&c.surf);

        int busy = (g_menu>=0) || g_about || g_ctx;

        /* child output: keep the scrolled-up view anchored as history grows */
        int before=vt.sb_count;
        int dirty=term_pump();
        int grew=vt.sb_count-before;
        if(sb_view>0){ sb_view+=grew; if(sb_view>vt.sb_count) sb_view=vt.sb_count; }

        /* keys -> shell (snap to the live tail); swallowed while a menu is open */
        int k,keyed=0; while((k=mx_key(&c))>=0){ if(!busy){ term_key(k); keyed=1; sb_view=0; } }

        int moved=(c.mx!=lmx||c.my!=lmy);
        int act = first||dirty||keyed||c.resized||c.mpressed||c.mreleased||c.rpressed||
                  moved||c.focused!=lfocus;
        lmx=c.mx; lmy=c.my; lfocus=c.focused;
        if(!act){ if(term_pid<0) break; sys_yield(); continue; }

        gfx_surface *s=&c.surf;

        /* ---- pointer: drag-select text, right-click menu ---- */
        if(!busy){
            if(c.mpressed && in_grid(c.mx,c.my)){       /* a grid click starts a fresh
                                                         * (collapsed) selection; menu /
                                                         * scrollbar clicks leave it be */
                int ln,col; pix_to_cell(c.mx,c.my,&ln,&col);
                sel_on=1; sel_a_ln=sel_c_ln=ln; sel_a_col=sel_c_col=col;
            }
            if(c.mdown && sel_on && (moved||c.mpressed) && c.my>=TMENU_H){
                int ln,col; pix_to_cell(c.mx,c.my,&ln,&col); sel_c_ln=ln; sel_c_col=col;
            }
            if(c.rpressed && in_grid(c.mx,c.my)){ g_ctx=1; g_ctx_x=c.mx; g_ctx_y=c.my; }
        }

        term_draw(s,c.focused);

        /* ---- scrollbar down the right strip (maps the scrollback view) ---- */
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        { int total=total_lines(), vis=vt.rows, vtop=vt.sb_count - sb_view;
          ui_ctx su=u; if(busy){ su.mpressed=su.mdown=su.mreleased=0; }
          ui_vscroll(&su,s,s->w-SBW,TMENU_H,SBW,s->h-TMENU_H,total,vis,&vtop);
          u.active=su.active; u.cur_id=su.cur_id;
          int nv=vt.sb_count - vtop; if(nv<0)nv=0; if(nv>vt.sb_count)nv=vt.sb_count;
          sb_view=nv; }

        /* ---- menu bar + About + right-click menu (overlays, drawn last) ---- */
        ui_menu_item eitems[]={{"Copy",T_COPY,has_sel(),"^C"},{"Paste",T_PASTE,1,"^V"},{0,0,0,0},{"Select All",T_SELALL,1,"^A"}};
        static const ui_menu_item hitems[]={{"About mxterm",T_ABOUT,1,0}};
        ui_menu menus[]={{"Edit",eitems,4},{"Help",hitems,1}};
        int mact=ui_menubar(&u,s,menus,2,&g_menu);
        if(mact) term_do(mact);

        static const char *al[]={"Makar terminal (mxterm)","(c) 2026 Arawn Davies  --  MIT","",
                                 "ANSI/VT100 emulator (vt100.c) over a forked shell.","Part of Makar OS."};
        ui_about(&u,s,"About mxterm",al,5,&g_about);

        ui_menu_item citems[]={{"Copy",T_COPY,has_sel(),"^C"},{"Paste",T_PASTE,1,"^V"},{0,0,0,0},{"Select All",T_SELALL,1,0}};
        int cact=ui_context_menu(&u,s,g_ctx_x,g_ctx_y,citems,4,&g_ctx);
        if(cact) term_do(cact);

        mx_present(&c);
        first=0;
        if(term_pid<0) break;
        sys_yield();
    }
    term_kill();
    mx_close(&c);
    return 0;
}
