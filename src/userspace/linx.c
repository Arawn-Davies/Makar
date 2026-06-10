/*
 * linx.c -- a full-screen, links/w3m-style text-mode web browser (freestanding
 * ring-3 ELF).  Reuses Makar's shared HTML engine (html.h: entity decode,
 * attribute extraction, URL resolution) and the userspace fetch layer
 * (web.h: HTTP/HTTPS over kernel sockets), reflows the page into a line buffer,
 * and paints it to the terminal with the cell API (sys_putch_at) -- a top menu
 * bar, the scrollable page with a navigable link cursor, and a bottom status
 * line.  Raw keys via the shared tkey.h, so it runs in a text VT AND inside an
 * mxterm window (the kernel cell-API -> ANSI bridge carries the output there).
 *
 *   arrows / PgUp / PgDn / Space   scroll        Tab / n / p   move link cursor
 *   Enter   follow the current link              g   go to URL    b   back
 *   r   reload      F9 / m   File menu            q / Ctrl-C   quit
 *
 * `linx --dump <url>` prints the reflowed page to stdout (pipe-friendly).
 */

#include "syscall.h"
#include "web.h"
#include "html.h"
#include "tkey.h"

#define URLCAP    1024
#define PAGECAP   (512*1024)
#define MAXLINES  3000
#define MAXCOLS   200
#define MAXLINKS  2048
#define LINKPOOL  (128*1024)
#define MAXCELLS  (MAXCOLS*80 + 4096)
#define TMPPAGE   "/tmp/linx.page"

/* VGA attribute bytes (links/w3m black-background look). */
#define CLR_TEXT    VGA_CLR(VGA_LGREY, VGA_BLACK)
#define CLR_BAR     VGA_CLR(VGA_BLACK, VGA_LGREY)   /* menu + status bars */
#define CLR_LINK    VGA_CLR(VGA_LCYAN, VGA_BLACK)
#define CLR_LINKCUR VGA_CLR(VGA_BLACK, VGA_LCYAN)   /* link under the cursor */
#define CLR_HEAD    VGA_CLR(VGA_YELLOW, VGA_BLACK)
#define CLR_MENUSEL VGA_CLR(VGA_WHITE, VGA_BLUE)

/* ---- tiny freestanding helpers ------------------------------------------- */
static int  sl(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static void scpy(char *d,const char *s,int max){ int i=0; if(max<=0)return; while(s&&s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static int  ieq(const char *a,const char *b){ int i=0; for(;;){ char x=lc(a[i]),y=lc(b[i]); if(x!=y)return 0; if(!x)return 1; i++; } }
static int  is_about(const char *s){ return lc(s[0])=='a'&&lc(s[1])=='b'&&lc(s[2])=='o'&&lc(s[3])=='u'&&lc(s[4])=='t'&&s[5]==':'; }
/* about: URLs are virtual (no scheme://) -- pass them through; everything else
 * goes through the shared normaliser (adds http:// to a bare host, etc.). */
static void to_url(const char *raw,char *out,int cap){ if(is_about(raw)) scpy(out,raw,cap); else html_url_normalise(raw,out,cap); }
static void puts1(const char *s){ if(s){int n=sl(s); if(n) sys_write(1,s,(unsigned)n);} }
static int  utos(unsigned v,char *o){ char t[12]; int i=0,n=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)o[n++]=t[--i]; o[n]=0; return n; }

/* ---- page model: reflowed lines + link spans ----------------------------- */
static char          g_html[PAGECAP];  static int g_htmllen;
static char          g_url[URLCAP];
static char          g_title[256];

static char          g_lines[MAXLINES][MAXCOLS];
static unsigned char g_head[MAXLINES];          /* 1 = heading line (yellow) */
static int           g_nlines;

typedef struct { int off, line, col, len; } linkent;  /* off into g_linkpool */
static linkent       g_link[MAXLINKS];  static int g_nlink;
static char          g_linkpool[LINKPOOL]; static int g_linkpooln;
static const char   *pool_at(int off){ return &g_linkpool[off]; }

/* ---- history ------------------------------------------------------------- */
#define HISTCAP 64
static char g_hist[HISTCAP][URLCAP]; static int g_histn;
static void hist_push(const char *u){
    if(!u||!u[0]) return;
    if(g_histn>=HISTCAP){ for(int i=1;i<HISTCAP;i++) scpy(g_hist[i-1],g_hist[i],URLCAP); g_histn--; }
    scpy(g_hist[g_histn++],u,URLCAP);
}

/* ---- terminal + cell buffer ---------------------------------------------- */
static int g_cols=80, g_rows=25, g_wrap=78;
static tty_cell_t g_cells[MAXCELLS]; static int g_ncells;

static void put_at(int col,int row,unsigned ch,unsigned clr){
    if(col<0||row<0||col>=g_cols||row>=g_rows) return;
    if(g_ncells>=MAXCELLS) return;
    g_cells[g_ncells].col=(unsigned char)col; g_cells[g_ncells].row=(unsigned char)row;
    g_cells[g_ncells].ch=(unsigned char)ch;   g_cells[g_ncells].clr=(unsigned char)clr;
    g_ncells++;
}
static void put_str(int col,int row,const char *s,unsigned clr){
    for(int i=0;s&&s[i];i++) put_at(col+i,row,(unsigned char)s[i],clr);
}
static void present(void){ sys_putch_at(g_cells,(unsigned)g_ncells); g_ncells=0; }

/* ---- file IO ------------------------------------------------------------- */
static int read_file(const char *path,char *buf,int cap){
    int fd=sys_open(path,O_RDONLY); if(fd<0) return -1;
    int total=0;
    for(;;){ long r=sys_read(fd,buf+total,(unsigned)(cap-1-total)); if(r<=0) break; total+=(int)r; if(total>=cap-1) break; }
    sys_close(fd); buf[total]=0; return total;
}
static const char *skip_to_close(const char *p,const char *end,const char *name){
    int nl_=sl(name);
    for(const char *q=p; q+nl_+3<=end; q++){
        if(q[0]=='<'&&q[1]=='/'){ int i=0; while(i<nl_&&lc(q[2+i])==lc(name[i])) i++;
            if(i==nl_){ const char *r=q+2+nl_; while(r<end&&*r!='>')r++; return (r<end)?r+1:end; } }
    }
    return end;
}
static void scan_title(const char *p,const char *end){
    g_title[0]=0;
    for(const char *q=p; q+8<end; q++){
        if(q[0]=='<'&&lc(q[1])=='t'&&lc(q[2])=='i'&&lc(q[3])=='t'&&lc(q[4])=='l'&&lc(q[5])=='e'){
            const char *s=q+6; while(s<end&&*s!='>')s++; if(s<end)s++;
            int o=0; while(s<end&&*s!='<'&&o<(int)sizeof(g_title)-1){ char c=*s; if(c=='\n'||c=='\r'||c=='\t')c=' '; g_title[o++]=c; s++; }
            while(o>0&&g_title[o-1]==' '){o--;} g_title[o]=0; return;
        }
    }
}

/* ---- reflow: HTML -> g_lines + g_link ------------------------------------ */
static int  r_col, r_blank, r_any, r_pend, r_head, r_inpre;
static int  l_active, l_idx, l_pending, l_line, l_col, l_mark;

static void rnewline(void){ if(g_nlines<MAXLINES) g_nlines++; r_col=0; }
static void rput(char ch){
    if(g_nlines==0) g_nlines=1;
    if(r_col>=g_wrap) rnewline();
    int li=g_nlines-1;
    if(li>=MAXLINES) return;
    if(r_col<MAXCOLS){ g_lines[li][r_col]=ch; if(r_head) g_head[li]=1; r_col++; r_any=1; r_blank=0;
        if(l_active){
            if(l_pending){ l_line=li; l_col=r_col-1; l_pending=0; g_link[l_idx].line=l_line; g_link[l_idx].col=l_col; }
            if(li==l_line) g_link[l_idx].len = r_col - l_col;
        }
    }
}
static void rbreak(int para){
    if(!r_any){ return; }
    if(r_col>0) rnewline();
    if(para && r_blank<1){ rnewline(); r_blank=1; }
    r_pend=0;
}
static void rword(const char *w,int n){
    if(n<=0) return;
    if(r_col==0){ /* line start: drop the leading space */ }
    else if(r_pend){ if(r_col+1+n>g_wrap) rnewline(); else rput(' '); }
    else if(r_col+n>g_wrap) rnewline();
    for(int i=0;i<n;i++) rput(w[i]);
    r_pend=0;
}
static int pool_put(const char *u){
    int n=sl(u); if(g_linkpooln+n+1>LINKPOOL) return -1;
    int off=g_linkpooln; for(int i=0;i<=n;i++) g_linkpool[g_linkpooln++]=u[i]; return off;
}

static void render(const char *html,int len,const char *base){
    const char *p=html, *end=html+len;
    for(int i=0;i<g_nlines && i<MAXLINES;i++){ for(int c=0;c<MAXCOLS;c++) g_lines[i][c]=0; g_head[i]=0; }
    g_nlines=1; r_col=0; r_blank=0; r_any=0; r_pend=0; r_head=0; r_inpre=0;
    g_nlink=0; g_linkpooln=0; l_active=0;
    for(int c=0;c<MAXCOLS;c++){ g_lines[0][c]=0; } g_head[0]=0;

    char wbuf[256]; int wn=0;
    #define FW() do{ if(wn>0){ rword(wbuf,wn); wn=0; } }while(0)

    while(p<end){
        char c=*p;
        if(c=='<'){
            if(p+3<end&&p[1]=='!'&&p[2]=='-'&&p[3]=='-'){ const char *q=p+4; while(q+2<end&&!(q[0]=='-'&&q[1]=='-'&&q[2]=='>'))q++; p=(q+2<end)?q+3:end; continue; }
            if(p+1<end&&p[1]=='!'){ while(p<end&&*p!='>')p++; if(p<end)p++; continue; }
            const char *t=p+1; int closing=0; if(t<end&&*t=='/'){closing=1;t++;}
            char name[16]; int ni=0;
            while(t<end&&ni<15&&((lc(*t)>='a'&&lc(*t)<='z')||(*t>='0'&&*t<='9'))) name[ni++]=lc(*t++);
            name[ni]=0;
            const char *attrs=t, *ae=t; while(ae<end&&*ae!='>')ae++;
            const char *next=(ae<end)?ae+1:end;
            FW();
            if(!closing&&(ieq(name,"script")||ieq(name,"style")||ieq(name,"head")||ieq(name,"svg")||ieq(name,"noscript"))){ p=skip_to_close(next,end,name); continue; }
            if(ieq(name,"br")) rbreak(0);
            else if(ieq(name,"hr")){ rbreak(0); int dn=g_wrap-1; for(int i=0;i<dn;i++) rput('-'); rbreak(0); }
            else if(ieq(name,"p")||ieq(name,"blockquote")) rbreak(1);
            else if(ieq(name,"div")||ieq(name,"section")||ieq(name,"article")||ieq(name,"header")||ieq(name,"footer")||ieq(name,"table")||ieq(name,"form")||ieq(name,"ul")||ieq(name,"ol")||ieq(name,"nav")||ieq(name,"main")||ieq(name,"aside")||ieq(name,"dl")) rbreak(0);
            else if(ieq(name,"tr")) rbreak(0);
            else if((ieq(name,"td")||ieq(name,"th"))&&!closing) r_pend=1;
            else if(ieq(name,"li")&&!closing){ rbreak(0); rput(' '); rput(' '); rput('*'); rput(' '); r_pend=0; }
            else if(name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&name[2]==0){ if(!closing){ rbreak(1); r_head=1; } else { r_head=0; rbreak(1); } }
            else if(ieq(name,"pre")){ r_inpre=!closing; rbreak(0); }
            else if(ieq(name,"title")){ p=next; continue; }
            else if(ieq(name,"a")){
                if(!closing){
                    char href[URLCAP];
                    if(html_attr_get(attrs,ae,"href",href,sizeof href)&&href[0]&&href[0]!='#'&&
                       !(lc(href[0])=='j'&&lc(href[1])=='a'&&lc(href[2])=='v'&&lc(href[3])=='a')){
                        char res[URLCAP]; html_url_resolve(base,href,res,sizeof res);
                        if(g_nlink<MAXLINKS){ l_mark=g_linkpooln; int off=pool_put(res);
                            if(off>=0){ g_link[g_nlink].off=off; g_link[g_nlink].line=0; g_link[g_nlink].col=0; g_link[g_nlink].len=0;
                                l_idx=g_nlink; g_nlink++; l_active=1; l_pending=1; } }
                    }
                } else if(l_active){ if(l_pending){ g_nlink--; g_linkpooln=l_mark; } l_active=0; }
            }
            else if(ieq(name,"img")&&!closing){
                char alt[128];
                if(html_attr_get(attrs,ae,"alt",alt,sizeof alt)&&alt[0]){ char b[140]; int bn=0; b[bn++]='['; for(int i=0;alt[i]&&bn<136;i++)b[bn++]=alt[i]; b[bn++]=']'; rword(b,bn); }
                else rword("[img]",5);
            }
            p=next; continue;
        }
        else if(c=='&'){ char eb[4]; const char *pp=p; int en=html_entity(&pp,end,eb);
            for(int i=0;i<en;i++){ char ec=eb[i]; if(ec==' '){ FW(); r_pend=1; } else if(wn<(int)sizeof(wbuf)) wbuf[wn++]=ec; }
            p=pp; continue; }
        else if(c==' '||c=='\t'||c=='\n'||c=='\r'){
            if(r_inpre){ FW(); if(c=='\n') rbreak(0); else if(c=='\t') rword("    ",4); else r_pend=1; }
            else { FW(); r_pend=1; }
            p++; continue;
        }
        else { if(wn<(int)sizeof(wbuf)) wbuf[wn++]=c; else { rword(wbuf,wn); wn=0; wbuf[wn++]=c; } p++; }
    }
    FW();
    if(l_active&&l_pending){ g_nlink--; g_linkpooln=l_mark; }
    #undef FW
}

/* ---- load a URL / path and reflow it ------------------------------------- */
static const char START_HTML[] =
    "<title>linx</title><h1>linx</h1>"
    "<p>A full-screen text-mode web browser for Makar. It reuses mxweb's HTML "
    "engine and reflows pages to the terminal with navigable links.</p>"
    "<p>Keys: arrows/PgUp/PgDn/Space scroll, Tab/n/p move the link cursor, Enter "
    "follows it, g go to a URL, b back, r reload, F9 menu, q quit.</p>"
    "<p>Try <a href=\"http://example.com/\">example.com</a> or press g to type a URL.</p>";

static int g_scroll, g_cur;     /* viewport scroll + current-link index (-1 none) */

static int load(const char *url){
    g_html[0]=0; g_htmllen=0;
    if(ieq(url,"about:linx")||ieq(url,"about:start")||!url[0]){
        scpy(g_url,"about:linx",URLCAP); g_htmllen=sl(START_HTML); scpy(g_html,START_HTML,PAGECAP);
    } else {
        const char *fp=0; char fb[URLCAP];
        if(html_has_scheme(url)){
            if(lc(url[0])=='f'&&lc(url[1])=='i'&&lc(url[2])=='l'&&lc(url[3])=='e'){ const char *q=url+4; if(*q==':')q++; while(*q=='/')q++; fb[0]='/'; scpy(fb+1,q,URLCAP-1); fp=fb; }
        } else if(url[0]=='/') fp=url;
        if(fp){ g_htmllen=read_file(fp,g_html,PAGECAP); if(g_htmllen<0) return -1; scpy(g_url,url,URLCAP); }
        else {
            int r=web_fetch(url,TMPPAGE); if(r<0) return r;
            g_htmllen=read_file(TMPPAGE,g_html,PAGECAP); if(g_htmllen<0) return -1; scpy(g_url,url,URLCAP);
        }
    }
    scan_title(g_html,g_html+g_htmllen);
    render(g_html,g_htmllen,g_url);
    g_scroll=0; g_cur = (g_nlink>0)?0:-1;
    return 0;
}

/* ---- drawing ------------------------------------------------------------- */
static void clamp_scroll(void){
    int maxs=g_nlines-(g_rows-2); if(maxs<0)maxs=0;
    if(g_scroll>maxs){g_scroll=maxs;} if(g_scroll<0){g_scroll=0;}
}
static void scroll_to_cur(void){
    if(g_cur<0||g_cur>=g_nlink) return;
    int li=g_link[g_cur].line, viewh=g_rows-2;
    if(li<g_scroll||li>=g_scroll+viewh){ g_scroll=li-viewh/2; clamp_scroll(); }
}
static void draw(void){
    g_ncells=0;
    int viewh=g_rows-2;
    /* menu bar */
    for(int c=0;c<g_cols;c++) put_at(c,0,' ',CLR_BAR);
    put_str(1,0,"linx",CLR_BAR);
    put_str(8,0,"File",CLR_BAR); put_str(15,0,"View",CLR_BAR); put_str(22,0,"Go",CLR_BAR); put_str(27,0,"Help",CLR_BAR);
    if(g_title[0]){ int tw=sl(g_title); int tx=g_cols-tw-1; if(tx>34) put_str(tx,0,g_title,CLR_BAR); }
    /* page text */
    for(int vr=0;vr<viewh;vr++){
        int li=g_scroll+vr, row=vr+1;
        unsigned char base=(li<g_nlines&&g_head[li])?CLR_HEAD:CLR_TEXT;
        for(int c=0;c<g_cols;c++){ char ch=(li<g_nlines&&c<MAXCOLS)?g_lines[li][c]:0; if(ch==0)ch=' '; put_at(c,row,(unsigned char)ch,base); }
    }
    /* link overlay (later cells win per (col,row)) */
    for(int k=0;k<g_nlink;k++){
        int li=g_link[k].line; if(li<g_scroll||li>=g_scroll+viewh) continue;
        int row=li-g_scroll+1; unsigned char cl=(k==g_cur)?CLR_LINKCUR:CLR_LINK;
        for(int c=g_link[k].col;c<g_link[k].col+g_link[k].len&&c<g_cols&&c<MAXCOLS;c++){
            char ch=g_lines[li][c]; if(ch==0)ch=' '; put_at(c,row,(unsigned char)ch,cl);
        }
    }
    /* status line: current link URL, else key hints */
    for(int c=0;c<g_cols;c++) put_at(c,g_rows-1,' ',CLR_BAR);
    if(g_cur>=0&&g_cur<g_nlink){ char st[256]; int n=0; st[n++]='>'; st[n++]=' ';
        const char *u=pool_at(g_link[g_cur].off); for(int i=0;u[i]&&n<(int)sizeof(st)-1;i++)st[n++]=u[i]; st[n]=0;
        put_str(1,g_rows-1,st,CLR_BAR);
    } else {
        char st[256]; int n=0; const char *u=g_url; for(int i=0;u[i]&&n<140;i++)st[n++]=u[i];
        const char *h="    (Tab links  Enter open  g url  b back  r reload  q quit)";
        for(int i=0;h[i]&&n<(int)sizeof(st)-1;i++){st[n++]=h[i];} st[n]=0;
        put_str(1,g_rows-1,st,CLR_BAR);
    }
    present();
    sys_set_cursor((unsigned)(g_cols-1),(unsigned)(g_rows-1));
}

/* ---- one-line prompt at the status row (returns 1=entered, 0=cancel) ----- */
static int prompt(const char *label,char *out,int cap){
    int row=g_rows-1,len=0; out[0]=0;
    for(;;){
        for(int c=0;c<g_cols;c++) put_at(c,row,' ',CLR_MENUSEL);
        put_str(1,row,label,CLR_MENUSEL);
        int base=1+sl(label)+1; put_str(base,row,out,CLR_MENUSEL);
        present(); sys_set_cursor((unsigned)(base+len),(unsigned)row);
        int k=tkey_get();
        if(k=='\n'||k=='\r') return 1;
        if(k==0x1B||k==KEY_CTRL_C) return 0;
        if(k==8||k==127){ if(len>0) out[--len]=0; continue; }
        if(k>=0x20&&k<0x7F&&len<cap-1){ out[len++]=(char)k; out[len]=0; }
    }
}

/* ---- navigation ---------------------------------------------------------- */
static void go(const char *raw){
    char u[URLCAP]; to_url(raw,u,sizeof u);
    hist_push(g_url);
    int r=load(u);
    if(r<0){ /* keep the old page; report on the status row */
        char m[160]; int n=0; const char *e="fetch failed: "; for(int i=0;e[i];i++)m[n++]=e[i];
        for(int i=0;u[i]&&n<150;i++){m[n++]=u[i];} m[n]=0;
        for(int c=0;c<g_cols;c++) put_at(c,g_rows-1,' ',VGA_CLR(VGA_WHITE,VGA_RED));
        put_str(1,g_rows-1,m,VGA_CLR(VGA_WHITE,VGA_RED)); present(); tkey_get();
        if(g_histn>0) g_histn--;   /* undo the push; we never left */
    }
}

int main(int argc,char **argv){
    int dump=0, ai=1;
    if(argc>=2&&(ieq(argv[1],"--dump")||ieq(argv[1],"-d"))){ dump=1; ai=2; }

    if(dump){
        if(ai>=argc){ puts1("usage: linx [--dump] <url|file>\n"); return 2; }
        g_cols=78; g_wrap=78;
        char start[URLCAP]; to_url(argv[ai],start,sizeof start);
        if(load(start)!=0){ puts1("linx: load failed\n"); return 1; }
        for(int i=0;i<g_nlines;i++){ int n=0; while(n<MAXCOLS&&g_lines[i][n])n++; sys_write(1,g_lines[i],(unsigned)n); sys_write(1,"\n",1); }
        if(g_nlink>0){ puts1("\n-- links --\n"); for(int k=0;k<g_nlink;k++){ char b[16]; int bn=0; b[bn++]='['; bn+=utos((unsigned)(k+1),b+bn); b[bn++]=']'; b[bn++]=' '; sys_write(1,b,(unsigned)bn); puts1(pool_at(g_link[k].off)); puts1("\n"); } }
        return 0;
    }

    /* full-screen mode */
    long ts=sys_term_size();
    g_cols=(int)((ts>>16)&0xFFFF); g_rows=(int)(ts&0xFFFF);
    if(g_cols<20||g_cols>MAXCOLS) g_cols=80;
    if(g_rows<6||g_rows>80) g_rows=25;
    g_wrap=g_cols-1; if(g_wrap>MAXCOLS-1) g_wrap=MAXCOLS-1;

    char start[URLCAP];
    if(argc>ai&&argv[ai][0]) to_url(argv[ai],start,sizeof start);
    else scpy(start,"about:linx",sizeof start);
    if(load(start)<0) load("about:linx");

    sys_tty_clear(CLR_TEXT);
    for(;;){
        draw();
        int k=tkey_get();
        if(k=='q'||k==KEY_CTRL_C) break;
        else if(k==KEY_ARROW_DOWN){ g_scroll++; clamp_scroll(); }
        else if(k==KEY_ARROW_UP){ g_scroll--; clamp_scroll(); }
        else if(k==KEY_PAGE_DOWN||k==' '){ g_scroll+=g_rows-3; clamp_scroll(); }
        else if(k==KEY_PAGE_UP){ g_scroll-=g_rows-3; clamp_scroll(); }
        else if(k=='\t'||k=='n'){ if(g_nlink>0){ g_cur=(g_cur+1)%g_nlink; scroll_to_cur(); } }
        else if(k=='p'||k=='N'){ if(g_nlink>0){ g_cur=(g_cur-1+g_nlink)%g_nlink; scroll_to_cur(); } }
        else if(k=='\n'||k=='\r'){ if(g_cur>=0&&g_cur<g_nlink){ char u[URLCAP]; scpy(u,pool_at(g_link[g_cur].off),URLCAP); go(u); } }
        else if(k=='g'||k=='G'){ char in[URLCAP]; if(prompt("Go to URL:",in,sizeof in)&&in[0]) go(in); }
        else if(k=='b'){ if(g_histn>0){ char prev[URLCAP]; scpy(prev,g_hist[--g_histn],URLCAP); load(prev); } }
        else if(k=='r'){ char cur[URLCAP]; scpy(cur,g_url,URLCAP); load(cur); }
        else if(k==KEY_F9||k=='m'||k=='M'){
            /* File menu popup */
            const char *items[]={"Go to URL","Back","Reload","Quit"}; int n=4,sel=0;
            for(;;){
                draw();
                int bx=8,by=1,bw=14,bh=n+2;
                for(int r=0;r<bh;r++) for(int c=0;c<bw;c++) put_at(bx+c,by+r,' ',CLR_BAR);
                for(int i=0;i<n;i++) put_str(bx+1,by+1+i,items[i],(i==sel)?CLR_MENUSEL:CLR_BAR);
                present();
                int mk=tkey_get(); int act=-1;
                if(mk==KEY_ARROW_UP){ if(sel>0)sel--; continue; }
                if(mk==KEY_ARROW_DOWN){ if(sel<n-1)sel++; continue; }
                if(mk=='\n'||mk=='\r') act=sel;
                else if(mk==0x1B) act=-1;
                else continue;
                if(act==0){ char in[URLCAP]; if(prompt("Go to URL:",in,sizeof in)&&in[0]) go(in); }
                else if(act==1){ if(g_histn>0){ char prev[URLCAP]; scpy(prev,g_hist[--g_histn],URLCAP); load(prev); } }
                else if(act==2){ char cur[URLCAP]; scpy(cur,g_url,URLCAP); load(cur); }
                else if(act==3){ sys_tty_clear(VGA_CLR(VGA_LGREY,VGA_BLACK)); return 0; }
                break;
            }
        }
    }
    sys_tty_clear(VGA_CLR(VGA_LGREY,VGA_BLACK));
    sys_set_cursor(0,0);
    return 0;
}
