/*
 * linx.c -- a lynx/links-style text-mode web browser (freestanding ring-3 ELF).
 *
 * Reuses Makar's shared HTML engine (html.h: entity decoding, attribute
 * extraction, URL resolution -- the same tokeniser core mxweb uses) and the
 * userspace fetch layer (web.h: HTTP/HTTPS over kernel TCP sockets, the same
 * web_fetch mxweb/wget call).  On top of that it reflows the page to the
 * terminal: whitespace-collapsed word wrap, ANSI styling, inline numbered [N]
 * link markers, and a lynx-style command prompt (type a number to follow a
 * link, b=back, r=reload, a URL to go, q=quit).
 *
 * Output is plain ANSI to stdout and input is read the bgetline way, so it
 * works in a text VT (cooked line discipline) and under mxterm (raw pipe).
 */

#include "syscall.h"
#include "web.h"
#include "html.h"

#define URLCAP    1024
#define PAGECAP   (512*1024)        /* max HTML rendered (larger is truncated) */
#define MAXLINKS  1024
#define LINKPOOL  (96*1024)
#define TMPPAGE   "/tmp/linx.page"

/* ---- ANSI ---------------------------------------------------------------- */
#define A_RESET "\x1b[0m"
#define A_BOLD  "\x1b[1m"
#define A_TITLE "\x1b[1;37m"
#define A_HEAD  "\x1b[1;32m"
#define A_LINK  "\x1b[36m"          /* anchor text: cyan        */
#define A_MARK  "\x1b[1;33m"        /* [N] marker: bold yellow  */
#define A_DIM   "\x1b[90m"

/* ---- tiny freestanding helpers ------------------------------------------- */
static int  sl(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static void scpy(char *d,const char *s,int max){ int i=0; if(max<=0)return; while(s&&s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static int  ieq(const char *a,const char *b){ int i=0; for(;;){ char x=lc(a[i]),y=lc(b[i]); if(x!=y)return 0; if(!x)return 1; i++; } }
static void puts1(const char *s){ if(s){ int n=sl(s); if(n) sys_write(1,s,(unsigned)n); } }
static void putn(const char *s,int n){ if(n>0) sys_write(1,s,(unsigned)n); }
static int  isdig(char c){ return c>='0'&&c<='9'; }
static int  atoin(const char *s){ int v=0; while(isdig(*s)) v=v*10+(*s++-'0'); return v; }

/* unsigned -> decimal into buf (returns length) */
static int utos(unsigned v,char *o){ char t[12]; int i=0,n=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)o[n++]=t[--i]; o[n]=0; return n; }

/* ---- page + link state --------------------------------------------------- */
static char g_html[PAGECAP];  static int g_htmllen;
static char g_url[URLCAP];                    /* current page URL */
static char g_title[256];

static char g_linkpool[LINKPOOL]; static int g_linkpooln;
static int  g_linkoff[MAXLINKS];  static int g_nlink;

static int add_link(const char *url){
    int n=sl(url);
    if (g_nlink>=MAXLINKS || g_linkpooln+n+1>LINKPOOL) return -1;
    g_linkoff[g_nlink]=g_linkpooln;
    for (int i=0;i<=n;i++) g_linkpool[g_linkpooln++]=url[i];
    return g_nlink++;
}
static const char *link_url(int i){ return (i>=0&&i<g_nlink)?&g_linkpool[g_linkoff[i]]:0; }

/* ---- history ------------------------------------------------------------- */
#define HISTCAP 64
static char g_hist[HISTCAP][URLCAP]; static int g_histn;
static void hist_push(const char *u){ if(!u||!u[0])return; if(g_histn>=HISTCAP){ for(int i=1;i<HISTCAP;i++) scpy(g_hist[i-1],g_hist[i],URLCAP); g_histn--; } scpy(g_hist[g_histn++],u,URLCAP); }

/* ---- reflow / word-wrap state -------------------------------------------- */
static int g_cols   = 78;     /* wrap width */
static int g_col    = 0;      /* current output column */
static int g_emitted= 0;      /* any visible content emitted yet */
static int g_blankrun=0;      /* trailing blank lines already emitted */
static int g_breakreq=0;      /* pending line breaks: 1=newline, 2=paragraph */
static int g_pend_space=0;    /* a separating space is pending */
static const char *g_prefix=0;/* line prefix to emit at the next line start */

static void nl(void){ sys_write(1,"\r\n",2); g_col=0; }

static void req_break(int n){ if(n>g_breakreq) g_breakreq=n; }

static void apply_break(void){
    if (!g_breakreq) return;
    if (g_emitted) {
        if (g_col>0) nl();
        if (g_breakreq>=2 && g_blankrun<1) { nl(); g_blankrun=1; }
    }
    g_breakreq=0;
    g_pend_space=0;             /* no leading space after a break */
}

/* Emit one visible word with whitespace-collapsing + wrapping. */
static void put_word(const char *w,int n){
    if (n<=0) return;
    apply_break();
    if (g_col==0) {
        if (g_prefix) { int pn=sl(g_prefix); putn(g_prefix,pn); g_col+=pn; g_prefix=0; }
    } else if (g_pend_space) {
        if (g_col+1+n > g_cols) nl();
        else { sys_write(1," ",1); g_col++; }
    } else if (g_col+n > g_cols) {
        nl();
    }
    if (g_col==0 && g_prefix) { int pn=sl(g_prefix); putn(g_prefix,pn); g_col+=pn; g_prefix=0; }
    putn(w,n);
    g_col += n;
    g_pend_space=0; g_emitted=1; g_blankrun=0;
}

/* ---- file IO ------------------------------------------------------------- */
static int read_file(const char *path,char *buf,int cap){
    int fd=sys_open(path,O_RDONLY);
    if (fd<0) return -1;
    int total=0;
    for(;;){
        long r=sys_read(fd,buf+total,(unsigned)(cap-1-total));
        if (r<=0) break;
        total+=(int)r;
        if (total>=cap-1) break;
    }
    sys_close(fd);
    buf[total]=0;
    return total;
}

/* ---- tag scanning helpers ------------------------------------------------ */
/* From `p` (just past a tag's '>'), find the matching "</name>" (case-
 * insensitive) and return a pointer just past it, or `end` if not found. */
static const char *skip_to_close(const char *p,const char *end,const char *name){
    int nl_=sl(name);
    for (const char *q=p; q+nl_+3<=end; q++) {
        if (q[0]=='<' && q[1]=='/') {
            int i=0; while(i<nl_ && lc(q[2+i])==lc(name[i])) i++;
            if (i==nl_) { const char *r=q+2+nl_; while(r<end && *r!='>') r++; return (r<end)?r+1:end; }
        }
    }
    return end;
}

/* ---- the renderer -------------------------------------------------------- */
static void scan_title(const char *p,const char *end){
    g_title[0]=0;
    for (const char *q=p; q+8<end; q++) {
        if (q[0]=='<' && lc(q[1])=='t' && lc(q[2])=='i' && lc(q[3])=='t' && lc(q[4])=='l' && lc(q[5])=='e') {
            const char *s=q+6; while(s<end && *s!='>') s++; if(s<end) s++;
            int o=0;
            while (s<end && *s!='<' && o<(int)sizeof(g_title)-1) {
                char c=*s;
                if (c=='\n'||c=='\r'||c=='\t') c=' ';
                g_title[o++]=c; s++;
            }
            while (o>0 && g_title[o-1]==' ') o--;
            g_title[o]=0;
            return;
        }
    }
}

static void render(const char *html,int len,const char *base){
    const char *p=html, *end=html+len;
    g_col=0; g_emitted=0; g_blankrun=0; g_breakreq=0; g_pend_space=0; g_prefix=0;
    g_nlink=0; g_linkpooln=0;

    char wbuf[256]; int wn=0;
    int in_pre=0, in_link=0;

    #define FLUSH_WORD() do{ if(wn>0){ put_word(wbuf,wn); wn=0; } }while(0)

    while (p<end) {
        char c=*p;
        if (c=='<') {
            /* comment / doctype */
            if (p+3<end && p[1]=='!' && p[2]=='-' && p[3]=='-') {
                const char *q=p+4; while(q+2<end && !(q[0]=='-'&&q[1]=='-'&&q[2]=='>')) q++;
                p = (q+2<end)? q+3 : end; continue;
            }
            if (p+1<end && p[1]=='!') { while(p<end && *p!='>') p++; if(p<end)p++; continue; }

            const char *t=p+1; int closing=0;
            if (t<end && *t=='/') { closing=1; t++; }
            char name[16]; int ni=0;
            while (t<end && ni<15 && ((lc(*t)>='a'&&lc(*t)<='z')||(*t>='0'&&*t<='9'))) name[ni++]=lc(*t++);
            name[ni]=0;
            const char *attrs=t;
            const char *ae=t; while(ae<end && *ae!='>') ae++;
            const char *next=(ae<end)?ae+1:end;

            FLUSH_WORD();

            if (!closing && (ieq(name,"script")||ieq(name,"style")||ieq(name,"head")||ieq(name,"svg")||ieq(name,"noscript"))) {
                p = skip_to_close(next,end,name); continue;
            }
            if (ieq(name,"br")) { FLUSH_WORD(); req_break(1); apply_break(); g_emitted=1; }
            else if (ieq(name,"hr")) {
                req_break(1); apply_break();
                puts1(A_DIM); int w=g_cols>4?g_cols-2:40; char dash[128]; int dn=w>(int)sizeof(dash)?(int)sizeof(dash):w;
                for(int i=0;i<dn;i++){ dash[i]='-'; } putn(dash,dn); puts1(A_RESET);
                g_col=dn; g_emitted=1; req_break(1);
            }
            else if (ieq(name,"p")||ieq(name,"div")||ieq(name,"section")||ieq(name,"article")||
                     ieq(name,"header")||ieq(name,"footer")||ieq(name,"table")||ieq(name,"form")||
                     ieq(name,"ul")||ieq(name,"ol")||ieq(name,"blockquote")||ieq(name,"nav")||
                     ieq(name,"main")||ieq(name,"aside")||ieq(name,"dl")) {
                req_break(ieq(name,"p")||ieq(name,"blockquote")?2:1);
            }
            else if (ieq(name,"tr")) { req_break(1); }
            else if ((ieq(name,"td")||ieq(name,"th")) && !closing) { g_pend_space=1; }
            else if (ieq(name,"li") && !closing) { req_break(1); apply_break(); g_prefix="  * "; }
            else if (ieq(name,"dt") && !closing) { req_break(1); }
            else if (ieq(name,"dd") && !closing) { req_break(1); g_prefix="    "; }
            else if (name[0]=='h' && name[1]>='1' && name[1]<='6' && name[2]==0) {
                if (!closing) { req_break(2); apply_break(); puts1(A_HEAD); }
                else { puts1(A_RESET); req_break(2); }
            }
            else if (ieq(name,"b")||ieq(name,"strong")) { puts1(closing?A_RESET:A_BOLD); }
            else if (ieq(name,"title")) { p=next; continue; }
            else if (ieq(name,"a")) {
                if (!closing) {
                    char href[URLCAP];
                    if (html_attr_get(attrs,ae,"href",href,sizeof href) && href[0] &&
                        href[0]!='#' && !ieq(href,"javascript:") &&
                        !(lc(href[0])=='j'&&lc(href[1])=='a'&&lc(href[2])=='v'&&lc(href[3])=='a')) {
                        char res[URLCAP]; html_url_resolve(base,href,res,sizeof res);
                        int idx=add_link(res);
                        if (idx>=0) {
                            char mk[16]; int mn=0; mk[mn++]='['; mn+=utos((unsigned)(idx+1),mk+mn); mk[mn++]=']';
                            puts1(A_MARK); put_word(mk,mn); puts1(A_LINK);
                            in_link=1;
                            g_pend_space=0;   /* anchor text attaches to the marker */
                        }
                    }
                } else if (in_link) { puts1(A_RESET); in_link=0; }
            }
            else if (ieq(name,"img") && !closing) {
                char alt[256];
                if (html_attr_get(attrs,ae,"alt",alt,sizeof alt) && alt[0]) {
                    char b[300]; int bn=0; b[bn++]='['; for(int i=0;alt[i]&&bn<290;i++) b[bn++]=alt[i]; b[bn++]=']';
                    puts1(A_DIM); put_word(b,bn); puts1(A_RESET);
                } else {
                    puts1(A_DIM); put_word("[img]",5); puts1(A_RESET);
                }
            }
            else if (ieq(name,"pre")) { in_pre=!closing; req_break(1); }
            p=next; continue;
        }
        else if (c=='&') {
            char ebuf[4]; const char *pp=p; int en=html_entity(&pp,end,ebuf);
            for (int i=0;i<en;i++){ char ec=ebuf[i];
                if (ec==' ') { FLUSH_WORD(); g_pend_space=1; }
                else if (wn<(int)sizeof(wbuf)) wbuf[wn++]=ec;
            }
            p=pp; continue;
        }
        else if (c==' '||c=='\t'||c=='\n'||c=='\r') {
            if (in_pre) {
                FLUSH_WORD();
                if (c=='\n') { req_break(1); apply_break(); }
                else if (c=='\t') { put_word("    ",4); }
                else { g_pend_space=1; }
            } else {
                FLUSH_WORD();
                g_pend_space=1;
            }
            p++; continue;
        }
        else {
            if (wn<(int)sizeof(wbuf)) wbuf[wn++]=c;
            else { put_word(wbuf,wn); wn=0; wbuf[wn++]=c; }
            p++;
        }
    }
    FLUSH_WORD();
    if (in_link) puts1(A_RESET);
    if (g_col>0) nl();
    #undef FLUSH_WORD
}

/* ---- page presentation --------------------------------------------------- */
static void show_header(void){
    puts1(A_TITLE);
    puts1(g_title[0]?g_title:"(untitled)");
    puts1(A_RESET); puts1("  "); puts1(A_DIM); puts1(g_url); puts1(A_RESET);
    nl();
    int w=g_cols; char dash[128]; int dn=w>(int)sizeof(dash)?(int)sizeof(dash):w;
    for(int i=0;i<dn;i++){ dash[i]='='; } puts1(A_DIM); putn(dash,dn); puts1(A_RESET);
    sys_write(1,"\r\n",2);
}

static void show_links_summary(void){
    if (g_nlink<=0) return;
    nl(); puts1(A_DIM); puts1("-- links --"); puts1(A_RESET); nl();
    int show=g_nlink>50?50:g_nlink;
    for (int i=0;i<show;i++){
        char num[16]; int nn=0; num[nn++]='['; nn+=utos((unsigned)(i+1),num+nn); num[nn++]=']'; num[nn++]=' '; num[nn]=0;
        puts1(A_MARK); putn(num,nn); puts1(A_RESET);
        puts1(link_url(i)); nl();
    }
    if (g_nlink>show) { puts1(A_DIM); puts1("  ... "); { char b[16]; int bn=utos((unsigned)(g_nlink-show),b); putn(b,bn);} puts1(" more"); puts1(A_RESET); nl(); }
}

/* ---- built-in start page ------------------------------------------------- */
static const char START_HTML[] =
    "<title>linx</title>"
    "<h1>linx</h1>"
    "<p>A text-mode web browser for Makar. It reuses mxweb's HTML engine and "
    "reflows pages to the terminal with numbered links.</p>"
    "<p>Commands at the <b>linx&gt;</b> prompt:</p>"
    "<ul>"
    "<li>a <b>number</b> follows that [N] link</li>"
    "<li><b>&lt;url&gt;</b> (or host.tld) opens a page</li>"
    "<li><b>b</b> back, <b>r</b> reload, <b>L</b> list links</li>"
    "<li><b>h</b> help, <b>q</b> quit</li>"
    "</ul>"
    "<p>Try: <a href=\"http://example.com/\">example.com</a></p>";

static void show_help(void){
    puts1(A_BOLD); puts1("linx commands:"); puts1(A_RESET); nl();
    puts1("  N         follow link number N (see [N] markers / 'L')\n");
    puts1("  <url>     open a URL or host (e.g. example.com)\n");
    puts1("  b         back        r   reload      L   list links\n");
    puts1("  u         show URL    h/? help        q   quit\n");
}

/* ---- load a URL/path and render it --------------------------------------- */
static int load(const char *url){
    g_html[0]=0; g_htmllen=0;

    if (ieq(url,"about:linx") || ieq(url,"about:start") || !url[0]) {
        scpy(g_url,"about:linx",URLCAP);
        g_htmllen=sl(START_HTML); scpy(g_html,START_HTML,PAGECAP);
    } else {
        /* local path? (no scheme + leading '/', or file://) */
        const char *fpath=0; char fbuf[URLCAP];
        if (html_has_scheme(url)) {
            if (lc(url[0])=='f'&&lc(url[1])=='i'&&lc(url[2])=='l'&&lc(url[3])=='e') {
                const char *q=url+4; if(*q==':')q++; while(*q=='/')q++; fbuf[0]='/'; scpy(fbuf+1,q,URLCAP-1); fpath=fbuf;
            }
        } else if (url[0]=='/') {
            fpath=url;
        }

        if (fpath) {
            g_htmllen=read_file(fpath,g_html,PAGECAP);
            if (g_htmllen<0) { puts1("linx: cannot open "); puts1(fpath); nl(); return -1; }
            scpy(g_url,url,URLCAP);
        } else {
            puts1(A_DIM); puts1("fetching "); puts1(url); puts1(" ..."); puts1(A_RESET); nl();
            int r=web_fetch(url,TMPPAGE);
            if (r<0) {
                puts1("linx: fetch failed (");
                if (r==-1) puts1("dns/connect/protocol");
                else if (r==-2) puts1("write error");
                else { puts1("http "); char b[16]; int bn=utos((unsigned)(-r),b); putn(b,bn); }
                puts1(")"); nl();
                return -1;
            }
            g_htmllen=read_file(TMPPAGE,g_html,PAGECAP);
            if (g_htmllen<0) { puts1("linx: cannot read fetched page\n"); return -1; }
            scpy(g_url,url,URLCAP);
        }
    }

    scan_title(g_html,g_html+g_htmllen);
    show_header();
    render(g_html,g_htmllen,g_url);
    return 0;
}

/* ---- bgetline: raw(mxterm pipe) or cooked(VT), self-echo only when raw ---- */
static int getline_(char *out,int cap){
    int len=0, raw=-1; char ch[64];
    for(;;){
        long n=sys_read(0,ch,sizeof ch);
        if (n<=0) return len?len:-1;
        if (raw<0){ raw=1; for(long i=0;i<n;i++) if(ch[i]=='\n'||ch[i]=='\r'){ raw=0; break; } }
        for(long i=0;i<n;i++){
            char c=ch[i];
            if (c=='\n'||c=='\r'){ out[len]=0; if(raw) sys_write(1,"\r\n",2); return len; }
            if (c==8||c==127){ if(len>0){ len--; if(raw) sys_write(1,"\b \b",3);} continue; }
            if (c>=32 && c<127 && len<cap-1){ out[len++]=c; if(raw) sys_write(1,&c,1); }
        }
    }
}

static const char *trim(const char *s){ while(*s==' '||*s=='\t') s++; return s; }
static int looks_like_url(const char *s){
    if (html_has_scheme(s)) return 1;
    if (s[0]=='/') return 1;
    for (int i=0;s[i];i++){ if(s[i]=='.'||s[i]=='/') return 1; if(s[i]==' ') break; }
    return 0;
}

int main(int argc,char **argv){
    unsigned cols=sys_term_cols();
    if (cols>=20 && cols<=400) g_cols=(int)cols-2; else g_cols=78;
    if (g_cols>100) g_cols=100;

    char start[URLCAP];
    if (argc>=2 && argv[1][0]) {
        /* --dump <url>: render once and exit (pipe-friendly) */
        int ai=1, dump=0;
        if (ieq(argv[1],"--dump")||ieq(argv[1],"-d")) { dump=1; ai=2; }
        if (ai>=argc) { puts1("usage: linx [--dump] <url|file>\n"); return dump?2:0; }
        html_url_normalise(argv[ai],start,sizeof start);
        if (load(start)!=0 && dump) return 1;
        if (dump) { show_links_summary(); puts1(A_RESET); return 0; }
    } else {
        scpy(start,"about:linx",sizeof start);
        load(start);
    }
    /* (the page body was already streamed by load(); print the link summary) */
    show_links_summary();

    /* interactive prompt loop */
    char line[URLCAP];
    for(;;){
        puts1(A_BOLD); puts1("linx> "); puts1(A_RESET);
        int n=getline_(line,sizeof line);
        if (n<0) break;
        const char *cmd=trim(line);
        if (!cmd[0]) { continue; }
        if (ieq(cmd,"q")||ieq(cmd,"quit")||ieq(cmd,"exit")) break;
        if (ieq(cmd,"h")||ieq(cmd,"?")||ieq(cmd,"help")) { show_help(); continue; }
        if (ieq(cmd,"u")||ieq(cmd,"url")) { puts1(g_url); nl(); continue; }
        if (ieq(cmd,"L")||ieq(cmd,"links")) { show_links_summary(); continue; }
        if (ieq(cmd,"r")||ieq(cmd,"reload")) { char cur[URLCAP]; scpy(cur,g_url,URLCAP); load(cur); show_links_summary(); continue; }
        if (ieq(cmd,"b")||ieq(cmd,"back")) {
            if (g_histn>0){ char prev[URLCAP]; scpy(prev,g_hist[--g_histn],URLCAP); load(prev); show_links_summary(); }
            else { puts1(A_DIM); puts1("(no history)"); puts1(A_RESET); nl(); }
            continue;
        }
        /* bare number -> follow link */
        int alldig=1; for(int i=0;cmd[i];i++) if(!isdig(cmd[i])){ alldig=0; break; }
        if (alldig) {
            int idx=atoin(cmd)-1; const char *u=link_url(idx);
            if (u){ char nu[URLCAP]; scpy(nu,u,URLCAP); hist_push(g_url); load(nu); show_links_summary(); }
            else { puts1(A_DIM); puts1("(no such link)"); puts1(A_RESET); nl(); }
            continue;
        }
        /* g/o <url>, or a bare URL/host */
        const char *tgt=cmd;
        if ((cmd[0]=='g'||cmd[0]=='o') && (cmd[1]==' ')) tgt=trim(cmd+1);
        if (looks_like_url(tgt)) {
            char nu[URLCAP]; html_url_normalise(tgt,nu,sizeof nu); hist_push(g_url); load(nu); show_links_summary();
        } else {
            puts1(A_DIM); puts1("unknown command (h for help)"); puts1(A_RESET); nl();
        }
    }
    puts1(A_RESET);
    return 0;
}
