/*
 * mxweb.elf -- a minimal, Dillo-like web browser, as a makx client.
 *
 * Phase 1: plain HTTP (via SYS_WGET) + a from-scratch HTML tokeniser and a
 * block/inline renderer -- headings, paragraphs, lists, bold, links, word-wrap,
 * horizontal rules and inline images (PNG/JPEG/BMP via the shared decoders).
 * No CSS and no JS yet (their content is stripped); HTTPS is a later phase and
 * is reported, not attempted (wget is HTTP-only).  The renderer re-flows to the
 * window width (a resizable makx window) and the whole thing is network-free
 * except on an explicit navigation (Go / Enter / a link / Reload / Back).
 *
 * It also opens a local .html file directly (the WM's default-app dispatch
 * hands mxweb a filesystem path for .htm / .html files -- see wm.c wm_open_path), so
 * relative <img>/<a> in a local page resolve against the file's directory.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "makx.h"
#include "img_png.h"
#include "img_jpg.h"
#include "img_bmp.h"

#define RGB GFX_RGB

/* ---- page palette (web pages read as dark-on-light; chrome stays dark) ---- */
#define PAGE_BG   RGB(0xf6,0xf6,0xf2)
#define COL_TEXT  RGB(0x18,0x1a,0x1e)
#define COL_HEAD  RGB(0x0a,0x0c,0x12)
#define COL_LINK  RGB(0x14,0x44,0xcc)
#define COL_RULE  RGB(0xc2,0xc6,0xcc)
#define COL_MUTE  RGB(0x80,0x86,0x90)

/* layout metrics */
#define GLYPH_W   8
#define LINE_BASE 14
#define SPACE_W   8
#define TOOLBAR_H 50
#define SBW       12          /* scrollbar width                              */
#define MARGIN    10

#define HTML_MAX  (1<<19)     /* 512 KiB page cap (demand-paged mmap)         */
#define POOL_MAX  65536       /* href string pool, rebuilt each render        */
#define MAX_LINKS 1024
#define MAX_IMGS  16
#define IMG_CAP_W 1024
#define IMG_CAP_H 1024
#define HIST_MAX  32
#define URLCAP    1024

/* ---- freestanding string helpers ---------------------------------------- */
static int  sl(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static void scpy(char *d,const char *s,int max){ int i=0; if(max<=0)return; while(s&&s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static int  seqi(const char *a,const char *b){ int i=0; for(;;){ char x=lc(a[i]),y=lc(b[i]); if(x!=y)return 0; if(!x)return 1; i++; } }
static int  starts_ci(const char *s,const char *pfx){ for(int i=0;pfx[i];i++){ if(lc(s[i])!=lc(pfx[i]))return 0; } return 1; }
static char *pcat(char *p,const char *s){ while(s&&*s)*p++=*s++; return p; }
static char *pnum(char *p,unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static int  has_scheme(const char *s){ for(int i=0;s[i]&&i<16;i++){ if(s[i]==':'&&s[i+1]=='/'&&s[i+2]=='/')return 1; if(s[i]=='/')return 0; } return 0; }

/* ---- globals ------------------------------------------------------------- */
static char *g_html;  static int g_html_len;
static char *g_pool;  static int g_pool_used;
static char  g_cur_url[URLCAP];
static char  g_urlbar[URLCAP];
static char  g_title[128];
static char  g_status[160];
static int   g_scroll, g_content_h;

typedef struct { int x,y,w,h; const char *href; } link_t;
static link_t g_links[MAX_LINKS]; static int g_nlink;

typedef struct { char key[256]; gfx_surface surf; int ok; } imgent;
static imgent g_imgs[MAX_IMGS]; static int g_nimg;

static char g_hist[HIST_MAX][URLCAP]; static int g_hist_n, g_hist_i = -1;

/* scrollbar drag state */
static int g_drag, g_drag_off;

/* ---- HTML form fields (text input -> search query) ----------------------- */
#define MAX_FIELDS 24
#define MAX_SUBS   12
typedef struct { char name[64]; char value[256]; int x, dy, w, h; unsigned char shown; } formfield;
static formfield g_fields[MAX_FIELDS]; static int g_nfields;
static int  g_ff = -1;                 /* focused field index (-1 = none)      */
static char g_form_action[URLCAP];     /* the first <form>'s action            */
typedef struct { int x, dy, w, h; } subrect;
static subrect g_subs[MAX_SUBS]; static int g_nsub;

/* the built-in start / home page, rendered offline (no network) */
static const char WELCOME[] =
    "<h1>mxweb</h1>"
    "<p>A minimal browser for Makar. Type an address above and press "
    "<b>Go</b> (or Enter).</p>"
    "<h3>What works</h3>"
    "<ul>"
    "<li>Plain <b>HTTP</b> pages (no TLS yet)</li>"
    "<li>Headings, paragraphs, lists, bold and horizontal rules</li>"
    "<li>Links &mdash; click to follow; <b>Back</b> / <b>Fwd</b> / <b>Home</b> navigate</li>"
    "<li>Inline images: PNG, JPEG and BMP</li>"
    "</ul>"
    "<h3>Not yet</h3>"
    "<p>CSS and JavaScript are ignored, and <b>HTTPS</b> is a later phase.</p>"
    "<hr>"
    "<p>You can also open a local <b>.html</b> file from the file manager.</p>";

/* ---- mmap helpers -------------------------------------------------------- */
static void *xmap(unsigned long len){
    void *p = sys_mmap(0, len, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    return (p==MAP_FAILED)?0:p;
}

/* Read an entire file into a fresh demand-paged mapping.  Returns the buffer
 * (caller munmaps) and writes the length; 0 on any failure.  Capped at 8 MiB. */
static unsigned char *read_file(const char *path, int *len){
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long sz = sys_lseek(fd, 0, SEEK_END); sys_lseek(fd, 0, SEEK_SET);
    if (sz <= 0 || sz > 8*1024*1024) { sys_close(fd); return 0; }
    unsigned char *buf = xmap((unsigned long)sz + 1);
    if (!buf) { sys_close(fd); return 0; }
    long total = 0, n;
    while (total < sz && (n = sys_read(fd, buf+total, (unsigned)(sz-total))) > 0) total += n;
    sys_close(fd);
    *len = (int)total;
    return buf;
}

/* ---- URL handling -------------------------------------------------------- */
/* Split a URL into scheme/host/path (path always begins with '/').  A bare
 * path (no scheme) yields empty scheme+host and path = the input. */
static void url_split(const char *u, char *scheme, char *host, char *path){
    scheme[0]=host[0]=0; path[0]=0;
    const char *p = u;
    int has = has_scheme(u);
    if (has) {
        int i=0; while(*p && *p!=':' && i<15) scheme[i++]=lc(*p++); scheme[i]=0;
        if (*p==':') p++;
        if (*p=='/') p++;
        if (*p=='/') p++;                                       /* skip "://" */
        i=0; while(*p && *p!='/' && i<255) host[i++]=*p++; host[i]=0;
    }
    if (*p=='/' ) scpy(path, p, URLCAP);
    else if (!has) scpy(path, u, URLCAP);      /* relative / local path */
    else { path[0]='/'; scpy(path+1, p, URLCAP-1); }
    if (!path[0]) { path[0]='/'; path[1]=0; }
}

/* Resolve `ref` (an href/src) against the current page URL into `out`. */
static void url_resolve(const char *ref, char *out, int cap){
    if (!ref || !ref[0]) { scpy(out, g_cur_url, cap); return; }
    if (has_scheme(ref)) { scpy(out, ref, cap); return; }
    char scheme[16], host[256], path[URLCAP];
    url_split(g_cur_url, scheme, host, path);
    char *o = out;
    if (ref[0]=='/' && ref[1]=='/') {                 /* protocol-relative */
        o = pcat(o, scheme[0]?scheme:"http"); o = pcat(o, ":"); o = pcat(o, ref);
    } else if (ref[0]=='/') {                          /* root-relative */
        if (scheme[0]) { o=pcat(o,scheme); o=pcat(o,"://"); o=pcat(o,host); }
        o = pcat(o, ref);
    } else {                                           /* same-dir relative */
        if (scheme[0]) { o=pcat(o,scheme); o=pcat(o,"://"); o=pcat(o,host); }
        /* directory part of the current path (up to the last '/') */
        int last=0; for(int i=0; path[i]; i++) if(path[i]=='/') last=i;
        for(int i=0;i<=last;i++) *o++=path[i];
        const char *r = ref; if (r[0]=='.'&&r[1]=='/') r+=2;   /* trim "./" */
        o = pcat(o, r);
    }
    *o = 0;
    (void)cap;
}

/* Normalise a user-typed address: add http:// when no scheme and not a local
 * absolute path, and give a bare host a "/" path. */
static void url_normalise(const char *in, char *out){
    while (*in==' '||*in=='\t') in++;
    char tmp[URLCAP];
    if (in[0]=='/' || has_scheme(in)) scpy(tmp, in, sizeof tmp);
    else { char *o=pcat(tmp,"http://"); scpy(o, in, (int)(sizeof tmp-7)); }
    /* strip trailing whitespace */
    int n=sl(tmp); while(n>0 && (tmp[n-1]==' '||tmp[n-1]=='\n'||tmp[n-1]=='\r')) tmp[--n]=0;
    /* a bare http://host with no path -> add "/" */
    if (has_scheme(tmp)) {
        char sc[16],ho[256],pa[URLCAP]; url_split(tmp,sc,ho,pa);
        char *o=pcat(out,sc); o=pcat(o,"://"); o=pcat(o,ho); o=pcat(o,pa); *o=0;
    } else scpy(out, tmp, URLCAP);
}

/* ---- entity decoding ----------------------------------------------------- */
static int udec(const char *s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v; }
static int uhex(const char *s){ int v=0; for(;;){ char c=lc(*s++); int d; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else break; v=v*16+d; } return v; }

/* Decode one entity starting at *pp (which points at '&').  Writes up to a few
 * bytes into out, returns the count, and advances *pp past the entity. */
static int entity(const char **pp, const char *end, char *out){
    const char *p = *pp;
    const char *s = p+1; char nm[16]; int k=0;
    while (s<end && *s!=';' && *s!='&' && *s!='<' && k<15 &&
           ((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='#')) nm[k++]=*s++;
    nm[k]=0;
    if (s<end && *s==';' && k>0) {
        *pp = s+1;
        if (nm[0]=='#') {
            int code = (nm[1]=='x'||nm[1]=='X') ? uhex(nm+2) : udec(nm+1);
            if (code==0xA0) { out[0]=' '; return 1; }
            if (code<128 && code>=32) { out[0]=(char)code; return 1; }
            if (code==0x2019||code==0x2018) { out[0]='\''; return 1; }
            if (code==0x201C||code==0x201D) { out[0]='"';  return 1; }
            if (code==0x2013||code==0x2014) { out[0]='-';  return 1; }
            out[0]='?'; return 1;
        }
        if (seqi(nm,"amp"))  { out[0]='&'; return 1; }
        if (seqi(nm,"lt"))   { out[0]='<'; return 1; }
        if (seqi(nm,"gt"))   { out[0]='>'; return 1; }
        if (seqi(nm,"quot")) { out[0]='"'; return 1; }
        if (seqi(nm,"apos")) { out[0]='\''; return 1; }
        if (seqi(nm,"nbsp")) { out[0]=' '; return 1; }
        if (seqi(nm,"copy")) { out[0]='(';out[1]='c';out[2]=')'; return 3; }
        if (seqi(nm,"reg"))  { out[0]='(';out[1]='r';out[2]=')'; return 3; }
        if (seqi(nm,"mdash")||seqi(nm,"ndash")) { out[0]='-'; return 1; }
        if (seqi(nm,"hellip")) { out[0]='.';out[1]='.';out[2]='.'; return 3; }
        if (seqi(nm,"rsquo")||seqi(nm,"lsquo")) { out[0]='\''; return 1; }
        if (seqi(nm,"rdquo")||seqi(nm,"ldquo")) { out[0]='"'; return 1; }
        return 0;                                  /* unknown named entity */
    }
    *pp = p+1; out[0]='&'; return 1;               /* a literal ampersand */
}

/* ---- attribute extraction ------------------------------------------------ */
/* Find attribute `name` in the tag-attr region [a,e) and copy its value into
 * out.  Returns 1 if found.  Handles "x", 'x' and bare values. */
static int attr_get(const char *a, const char *e, const char *name, char *out, int cap){
    int nl = sl(name);
    for (const char *p=a; p+nl < e; p++) {
        if (p!=a && p[-1]!=' ' && p[-1]!='\t' && p[-1]!='\n') continue;
        int i=0; while(i<nl && lc(p[i])==lc(name[i])) i++;
        if (i!=nl) continue;
        const char *q = p+nl; while(q<e && (*q==' '||*q=='\t')) q++;
        if (q>=e || *q!='=') continue;
        q++; while(q<e && (*q==' '||*q=='\t')) q++;
        char quote = 0; if (q<e && (*q=='"'||*q=='\'')) quote=*q++;
        int o=0;
        while (q<e && o<cap-1) {
            if (quote) { if(*q==quote) break; }
            else if (*q==' '||*q=='\t'||*q=='>') break;
            out[o++]=*q++;
        }
        out[o]=0;
        return 1;
    }
    return 0;
}

/* ---- image cache --------------------------------------------------------- */
static void reset_images(void){
    for (int i=0;i<g_nimg;i++)
        if (g_imgs[i].ok && g_imgs[i].surf.px)
            sys_munmap(g_imgs[i].surf.px, (unsigned long)IMG_CAP_W*IMG_CAP_H*4);
    g_nimg = 0;
}
static gfx_surface *img_find(const char *src){
    for (int i=0;i<g_nimg;i++) if (seqi(g_imgs[i].key, src)) return g_imgs[i].ok?&g_imgs[i].surf:0;
    return 0;
}

/* Fetch + decode one image (resolved URL or local path) into a surface. */
static int img_decode(const char *resolved, gfx_surface *out){
    const char *fpath;
    if (has_scheme(resolved)) {
        if (sys_wget(resolved, "/tmp/mxweb.img") < 0) return -1;
        fpath = "/tmp/mxweb.img";
    } else {
        fpath = resolved;
        if (resolved[0]=='f'&&resolved[1]=='i'&&resolved[2]=='l'&&resolved[3]=='e') {
            const char *s=resolved; while(*s&&*s!='/')s++; while(*s=='/')s++; fpath=s-1;  /* file:///x -> /x */
        }
    }
    int fl=0; unsigned char *fb = read_file(fpath, &fl);
    if (!fb || fl < 4) { if(fb) sys_munmap(fb,(unsigned long)fl+1); return -1; }
    gfx_u32 *px = xmap((unsigned long)IMG_CAP_W*IMG_CAP_H*4);
    if (!px) { sys_munmap(fb,(unsigned long)fl+1); return -1; }
    int w=0,h=0, rc=-1;
    if (fb[0]==0x89 && fb[1]=='P' && fb[2]=='N' && fb[3]=='G')
        rc = png_decode(fb,(unsigned)fl,px,IMG_CAP_W,IMG_CAP_H,&w,&h,0,0);
    else if (fb[0]==0xFF && fb[1]==0xD8)
        rc = jpg_decode(fb,(unsigned)fl,px,IMG_CAP_W,IMG_CAP_H,&w,&h,0,0);
    else if (fb[0]=='B' && fb[1]=='M')
        rc = bmp_decode(fb,(unsigned)fl,px,IMG_CAP_W,IMG_CAP_H,&w,&h);
    sys_munmap(fb,(unsigned long)fl+1);
    if (rc!=0 || w<=0 || h<=0) { sys_munmap(px,(unsigned long)IMG_CAP_W*IMG_CAP_H*4); return -1; }
    out->px=px; out->w=w; out->h=h;
    return 0;
}

/* Scan the loaded page for <img src> and pre-decode each (once, on navigate). */
static void scan_images(void){
    const char *p=g_html, *end=g_html+g_html_len;
    while (p<end && g_nimg<MAX_IMGS) {
        if (*p!='<') { p++; continue; }
        const char *q=p+1; if(*q=='/')q++;
        if (!(lc(q[0])=='i'&&lc(q[1])=='m'&&lc(q[2])=='g')) { p++; continue; }
        const char *a=q+3, *e=a; while(e<end && *e!='>') e++;
        char src[256];
        if (attr_get(a,e,"src",src,sizeof src) && src[0]) {
            int dup=0; for(int i=0;i<g_nimg;i++) if(seqi(g_imgs[i].key,src)){dup=1;break;}
            if (!dup) {
                imgent *ie=&g_imgs[g_nimg];
                scpy(ie->key, src, sizeof ie->key);
                char res[URLCAP]; url_resolve(src,res,sizeof res);
                ie->ok = (img_decode(res,&ie->surf)==0);
                g_nimg++;
            }
        }
        p = (e<end)?e+1:end;
    }
}

/* Extract <title> for the chrome strip (once, on navigate). */
static void scan_title(void){
    g_title[0]=0;
    const char *p=g_html, *end=g_html+g_html_len;
    while (p<end) {
        if (*p=='<' && lc(p[1])=='t'&&lc(p[2])=='i'&&lc(p[3])=='t'&&lc(p[4])=='l'&&lc(p[5])=='e') {
            const char *q=p+6; while(q<end&&*q!='>')q++; if(q<end)q++;
            int o=0; while(q<end && *q!='<' && o<127){ char c=*q++; if(c=='\n'||c=='\t'||c=='\r')c=' '; if(c==' '&&(o==0||g_title[o-1]==' ')) continue; g_title[o++]=c; }
            while(o>0&&g_title[o-1]==' ')o--;
            g_title[o]=0;
            return;
        }
        p++;
    }
}

/* ---- minimal CSS: color / background[-color] / font-weight from <style> + style= --- */
typedef struct { char tag[12], cls[40], id[40]; gfx_u32 fg, bg; int width; unsigned char hfg, hbg, bold, scale, center; } cssrule;
#define MAX_CSS 128
static cssrule g_css[MAX_CSS]; static int g_ncss;
/* page-level CSS picked out for the whole document: a centred max-width card
 * (div with width + margin:auto) and the body background. */
static int     g_card_w;
static gfx_u32 g_card_bg, g_body_bg;
static unsigned char g_has_card, g_has_body_bg;
static void css_reset(void){ g_ncss=0; g_card_w=0; g_has_card=0; g_has_body_bg=0; }

static int hexd(char c){ c=lc(c); if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; }

/* Parse the first colour token of a CSS value (#rgb, #rrggbb, or a common name). */
static int css_color(const char *v,int n,gfx_u32 *out){
    int i=0; while(i<n && (v[i]==' '||v[i]=='\t')) i++;
    if (i<n && v[i]=='#') {
        i++; int d[6],k=0; while(i<n && k<6){ int h=hexd(v[i]); if(h<0)break; d[k++]=h; i++; }
        if (k>=6){ *out=RGB(d[0]*16+d[1], d[2]*16+d[3], d[4]*16+d[5]); return 1; }
        if (k==3){ *out=RGB(d[0]*17, d[1]*17, d[2]*17); return 1; }
        return 0;
    }
    if (i+2<n && lc(v[i])=='r'&&lc(v[i+1])=='g'&&lc(v[i+2])=='b') {   /* rgb()/rgba() */
        const char *q=v+i; while (q<v+n && *q!='(') q++;
        if (q<v+n) {
            q++; int comp[3]={0,0,0}, ci=0;
            while (q<v+n && *q!=')' && ci<3) {
                while (q<v+n && (*q==' '||*q==',')) q++;
                int val=0, any=0; while (q<v+n && *q>='0'&&*q<='9'){ val=val*10+(*q-'0'); q++; any=1; }
                if (any) comp[ci++]=val;
                while (q<v+n && *q!=',' && *q!=')') q++;
            }
            *out=RGB(comp[0]&0xff, comp[1]&0xff, comp[2]&0xff); return 1;
        }
    }
    static const struct { const char *n; gfx_u32 c; } NM[] = {
        {"white",RGB(255,255,255)},{"black",RGB(0,0,0)},{"red",RGB(204,0,0)},
        {"green",RGB(0,128,0)},{"blue",RGB(0,0,238)},{"navy",RGB(0,0,128)},
        {"gray",RGB(128,128,128)},{"grey",RGB(128,128,128)},{"silver",RGB(192,192,192)},
        {"lightgray",RGB(211,211,211)},{"lightgrey",RGB(211,211,211)},{"gainsboro",RGB(220,220,220)},
        {"whitesmoke",RGB(245,245,245)},{"darkgray",RGB(169,169,169)},{"darkgrey",RGB(169,169,169)},
        {"dimgray",RGB(105,105,105)},{"dimgrey",RGB(105,105,105)},{"maroon",RGB(128,0,0)},
        {"olive",RGB(128,128,0)},{"teal",RGB(0,128,128)},{"purple",RGB(128,0,128)},
        {"orange",RGB(255,165,0)},{"yellow",RGB(255,255,0)},{"lime",RGB(0,205,0)},
        {"aqua",RGB(0,255,255)},{"fuchsia",RGB(255,0,255)},
    };
    char w[24]; int k=0; while(i<n && k<23 && ((v[i]>='a'&&v[i]<='z')||(v[i]>='A'&&v[i]<='Z'))){ w[k++]=lc(v[i]); i++; } w[k]=0;
    for (unsigned j=0;j<sizeof NM/sizeof NM[0];j++) if (seqi(w,NM[j].n)){ *out=NM[j].c; return 1; }
    return 0;
}

/* A fixed pixel width (e.g. "800px"); 0 for "%", "auto", or no unit we honour. */
static int css_px(const char *v,int n){
    int i=0; while(i<n && (v[i]==' '||v[i]=='\t')) i++;
    int val=0, any=0; while(i<n && v[i]>='0'&&v[i]<='9'){ val=val*10+(v[i]-'0'); i++; any=1; }
    if(!any) return 0;
    while(i<n && v[i]==' ') i++;
    if (i<n && v[i]=='%') return 0;
    return val;
}
/* Map a font-size to an integer glyph scale (1/2/3) -- %, px, pt, em, keywords. */
static int css_fontscale(const char *v,int n){
    int i=0; while(i<n && (v[i]==' '||v[i]=='\t')) i++;
    if (i<n && ((v[i]>='a'&&v[i]<='z')||(v[i]>='A'&&v[i]<='Z'))) {
        char w[16]; int k=0; while(i<n&&k<15&&((v[i]>='a'&&v[i]<='z')||(v[i]>='A'&&v[i]<='Z')||v[i]=='-')){ w[k++]=lc(v[i]); i++; } w[k]=0;
        if (seqi(w,"xx-large")||seqi(w,"x-large")) return 3;
        if (seqi(w,"large")||seqi(w,"larger")) return 2;
        return 1;
    }
    int val=0, any=0; while(i<n && v[i]>='0'&&v[i]<='9'){ val=val*10+(v[i]-'0'); i++; any=1; }
    if(!any) return 0;
    int pct;
    if (i<n && v[i]=='%') pct=val;
    else if (i+1<n && lc(v[i])=='e'&&lc(v[i+1])=='m') pct=val*100;
    else if (i+1<n && lc(v[i])=='p'&&lc(v[i+1])=='x') pct=val*100/16;
    else if (i+1<n && lc(v[i])=='p'&&lc(v[i+1])=='t') pct=val*100/12;
    else pct=val;
    return pct>=160?3 : pct>=128?2 : 1;
}

/* Parse "prop:value;..." declarations into r (colour / background / weight / size / width / margin). */
static void css_decls(cssrule *r, const char *b, const char *e){
    while (b<e) {
        while (b<e && (*b==' '||*b=='\t'||*b=='\n'||*b=='\r'||*b==';')) b++;
        const char *ps=b; while (b<e && *b!=':' && *b!=';' && *b!='}') b++;
        if (b>=e || *b!=':') { while(b<e && *b!=';') b++; continue; }
        const char *pe=b; b++;
        const char *vs=b; while (b<e && *b!=';') b++;
        char prop[24]; int pi=0; for(const char*q=ps;q<pe && pi<23;q++) if(*q!=' '&&*q!='\t') prop[pi++]=lc(*q); prop[pi]=0;
        int vn=(int)(b-vs); gfx_u32 col;
        if (seqi(prop,"color")) { if (css_color(vs,vn,&col)){ r->fg=col; r->hfg=1; } }
        else if (seqi(prop,"background")||seqi(prop,"background-color")) { if (css_color(vs,vn,&col)){ r->bg=col; r->hbg=1; } }
        else if (seqi(prop,"font-weight")) { for(const char*q=vs;q<b;q++){ if(*q=='b'||(*q>='6'&&*q<='9')){ r->bold=1; break; } } }
        else if (seqi(prop,"font-size")) { int sc=css_fontscale(vs,vn); if(sc) r->scale=(unsigned char)sc; }
        else if (seqi(prop,"width")) { int px=css_px(vs,vn); if(px>0) r->width=px; }
        else if (seqi(prop,"margin")||seqi(prop,"margin-left")||seqi(prop,"margin-right")) {
            for(const char*q=vs;q+2<b;q++) if(lc(q[0])=='a'&&lc(q[1])=='u'&&lc(q[2])=='t'){ r->center=1; break; }
        }
    }
}

/* Parse a <style> block into g_css (uses the last compound of each selector). */
static void css_parse_block(const char *p, const char *end){
    while (p<end && g_ncss<MAX_CSS) {
        while (p<end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
        if (p>=end) break;
        if (*p=='@') {                       /* skip @media / @font-face / @import */
            int depth=0; while(p<end){ if(*p=='{')depth++; else if(*p=='}'){ if(depth)depth--; if(!depth){p++;break;} } else if(*p==';'&&!depth){p++;break;} p++; }
            continue;
        }
        const char *sel=p; while (p<end && *p!='{' && *p!='}') p++;
        if (p>=end || *p!='{') break;
        const char *selend=p; p++;
        const char *body=p; while (p<end && *p!='}') p++;
        const char *bodyend=p; if (p<end) p++;
        cssrule tmpl; tmpl.tag[0]=tmpl.cls[0]=tmpl.id[0]=0; tmpl.fg=tmpl.bg=0; tmpl.width=0; tmpl.hfg=tmpl.hbg=tmpl.bold=tmpl.scale=tmpl.center=0;
        css_decls(&tmpl, body, bodyend);
        /* a centred fixed-width container (width + margin:auto) -> the page card */
        if (tmpl.width>0 && tmpl.center && !g_has_card) { g_card_w=tmpl.width; g_card_bg=tmpl.hbg?tmpl.bg:RGB(255,255,255); g_has_card=1; }
        if (!(tmpl.hfg||tmpl.hbg||tmpl.bold||tmpl.scale)) continue;
        const char *ss=sel;
        while (ss<selend && g_ncss<MAX_CSS) {
            while (ss<selend && (*ss==','||*ss==' '||*ss=='\t'||*ss=='\n')) ss++;
            const char *se=ss; while (se<selend && *se!=',') se++;
            /* trim trailing whitespace (the gap before '{') so the last compound
             * isn't an empty run -- otherwise `div.banner {` parsed to an empty
             * selector and the rule was dropped. */
            const char *ce=se; while (ce>ss && (ce[-1]==' '||ce[-1]=='\t'||ce[-1]=='\n'||ce[-1]=='\r')) ce--;
            const char *cstart=ss; for(const char*q=ss;q<ce;q++) if(*q==' '||*q=='>'||*q=='+'||*q=='~') cstart=q+1;
            cssrule r=tmpl; int ti=0,ci=0,ii=0,mode=0;
            for (const char *q=cstart;q<ce;q++){
                char ch=*q;
                if (ch==':') break;
                if (ch=='.'){ mode=1; continue; }
                if (ch=='#'){ mode=2; continue; }
                if (ch=='*'){ continue; }
                if (mode==0){ if(ti<11) r.tag[ti++]=lc(ch); }
                else if (mode==1){ if(ci<39) r.cls[ci++]=ch; }
                else { if(ii<39) r.id[ii++]=ch; }
            }
            r.tag[ti]=0; r.cls[ci]=0; r.id[ii]=0;
            if (seqi(r.tag,"body") && r.hbg) { g_body_bg=r.bg; g_has_body_bg=1; }
            if (ti||ci||ii) g_css[g_ncss++]=r;
            ss = se+1;
        }
    }
}

/* Collect every <style> block in the page into g_css (once, on navigate). */
static void scan_css(void){
    css_reset();
    const char *p=g_html, *end=g_html+g_html_len;
    while (p<end && g_ncss<MAX_CSS) {
        if (*p=='<' && lc(p[1])=='s'&&lc(p[2])=='t'&&lc(p[3])=='y'&&lc(p[4])=='l'&&lc(p[5])=='e') {
            const char *q=p+6; while(q<end&&*q!='>')q++; if(q<end)q++;
            const char *cs=q;
            while (q+7<=end && !(q[0]=='<'&&q[1]=='/'&&lc(q[2])=='s'&&lc(q[3])=='t'&&lc(q[4])=='y'&&lc(q[5])=='l'&&lc(q[6])=='e')) q++;
            css_parse_block(cs, q);
            p=q; continue;
        }
        p++;
    }
}

static int cls_has(const char *attr,const char *cls){
    if (!cls[0]) return 1;
    int cl=sl(cls);
    for (const char *p=attr; *p; ) {
        while (*p==' '||*p=='\t') p++;
        const char *s=p; while (*p && *p!=' ' && *p!='\t') p++;
        if ((p-s)==cl) { int k=0; for(;k<cl;k++) if(s[k]!=cls[k]) break; if(k==cl) return 1; }
    }
    return 0;
}

/* Resolve the CSS for one element (tag + class attr + id) into fg/bg/bold. */
static void css_match(const char *tag,const char *classattr,const char *id,
                      int *hfg,gfx_u32 *fg,int *hbg,gfx_u32 *bg,int *bold,int *scale){
    int sf=-1,sb=-1,sd=-1,ss=-1;
    for (int i=0;i<g_ncss;i++) {
        cssrule *r=&g_css[i];
        if (r->tag[0] && !seqi(r->tag,tag)) continue;
        if (r->cls[0] && !cls_has(classattr,r->cls)) continue;
        if (r->id[0]  && !(id && seqi(r->id,id))) continue;
        int spec=(r->id[0]?100:0)+(r->cls[0]?10:0)+(r->tag[0]?1:0);
        if (r->hfg  && spec>=sf){ *hfg=1;  *fg=r->fg; sf=spec; }
        if (r->hbg  && spec>=sb){ *hbg=1;  *bg=r->bg; sb=spec; }
        if (r->bold && spec>=sd){ *bold=1; sd=spec; }
        if (r->scale && spec>=ss){ *scale=r->scale; ss=spec; }
    }
}

/* ---- layout state -------------------------------------------------------- */
typedef struct {
    gfx_surface *s;
    int x0, y0;          /* content origin (surface coords)                  */
    int cw, vh;          /* content width / viewport height                  */
    int cx, cy;          /* pen, document coords (cy grows down from 0)      */
    int line_h, indent;
    int scroll;
    int need_sp, link, pre, ol, ord;
    const char *href;    /* current <a> target (into g_pool)                 */
    /* CSS + UA style stack, by element nesting */
    struct { char tag[12]; int hfg; gfx_u32 fg; int hbg; gfx_u32 bg; int bold; int scale; } st[32];
    int sd;
    gfx_u32 cur_fg, cur_bg; int cur_fg_on, cur_bg_on, cur_bold, cur_scale;
    int bg_line;         /* doc-y already background-filled (once per line)   */
    int field_n;         /* running count of <input> fields drawn this walk   */
} Lay;

static int  is_void(const char *n){ return seqi(n,"img")||seqi(n,"br")||seqi(n,"hr")||seqi(n,"input")||seqi(n,"meta")||seqi(n,"link")||seqi(n,"col")||seqi(n,"base")||seqi(n,"area")||seqi(n,"source")||seqi(n,"track")||seqi(n,"wbr")||seqi(n,"embed")||seqi(n,"param"); }
static int  autocloses(const char *n){ return seqi(n,"li")||seqi(n,"p")||seqi(n,"td")||seqi(n,"th")||seqi(n,"tr")||seqi(n,"dd")||seqi(n,"dt")||seqi(n,"option"); }
static int  line_base(Lay *L){ return 8*L->cur_scale + 6; }

static void lay_recompute(Lay *L){
    L->cur_fg=COL_TEXT; L->cur_fg_on=0; L->cur_bold=0; L->cur_bg_on=0; L->cur_scale=1;
    for (int i=0;i<L->sd;i++){
        if (L->st[i].hfg){ L->cur_fg=L->st[i].fg; L->cur_fg_on=1; }
        if (L->st[i].bold) L->cur_bold=1;
        if (L->st[i].hbg){ L->cur_bg_on=1; L->cur_bg=L->st[i].bg; }
        if (L->st[i].scale>1) L->cur_scale=L->st[i].scale;
    }
}
static void lay_push(Lay *L,const char *name,const char *a,const char *e){
    if (L->sd>=32) return;
    int hfg=0,hbg=0,bold=0; gfx_u32 fg=0,bg=0;
    char cls[80]={0}, id[48]={0};
    attr_get(a,e,"class",cls,sizeof cls);
    attr_get(a,e,"id",id,sizeof id);
    int cssscale=0;
    if (g_ncss) css_match(name,cls,id,&hfg,&fg,&hbg,&bg,&bold,&cssscale);
    int heading = (name[0]=='h'&&name[1]>='1'&&name[1]<='6'&&!name[2]);
    if (heading||seqi(name,"b")||seqi(name,"strong")||seqi(name,"th")) bold=1;
    int scale=1; if (heading){ int lv=name[1]-'0'; scale=(lv==1)?3:(lv<=3)?2:1; }
    if (cssscale>scale) scale=cssscale;
    char stv[192];
    if (attr_get(a,e,"style",stv,sizeof stv)) {
        cssrule r; r.tag[0]=r.cls[0]=r.id[0]=0; r.fg=fg; r.bg=bg; r.width=0; r.scale=0; r.center=0;
        r.hfg=(unsigned char)hfg; r.hbg=(unsigned char)hbg; r.bold=(unsigned char)bold;
        css_decls(&r, stv, stv+sl(stv));
        fg=r.fg; bg=r.bg; hfg=r.hfg; hbg=r.hbg; bold=r.bold;
        if (r.scale>scale) scale=r.scale;
    }
    int i=L->sd++;
    scpy(L->st[i].tag,name,12);
    L->st[i].hfg=hfg; L->st[i].fg=fg; L->st[i].hbg=hbg; L->st[i].bg=bg; L->st[i].bold=bold; L->st[i].scale=scale;
    lay_recompute(L);
}
static void lay_pop(Lay *L,const char *name){
    for (int i=L->sd-1;i>=0;i--) if (seqi(L->st[i].tag,name)){ L->sd=i; break; }
    lay_recompute(L);
}

static void draw_chars(gfx_surface *s,int x,int y,const char *str,int n,gfx_u32 fg,int bold,int xmax,int scale){
    int gw=GLYPH_W*(scale>1?scale:1);
    for (int i=0;i<n;i++){ if(x+gw>xmax) break; unsigned char c=(unsigned char)str[i]; gfx_char_scaled(s,x,y,c,fg,scale); if(bold) gfx_char_scaled(s,x+1,y,c,fg,scale); x+=gw; }
}

static void add_link(Lay *L,int sx,int sy_doc,int w,int h){
    if (g_nlink>=MAX_LINKS || !L->href) return;
    g_links[g_nlink].x=sx; g_links[g_nlink].y=sy_doc; g_links[g_nlink].w=w; g_links[g_nlink].h=h;
    g_links[g_nlink].href=L->href; g_nlink++;
}

static void newline(Lay *L,int extra){ L->cy += L->line_h + extra; L->line_h = line_base(L); L->cx = L->indent; L->need_sp = 0; }
static void block(Lay *L,int gap){ if (L->cx>L->indent) newline(L,0); L->cy += gap; }

/* Fill the current line's background strip (once) when a styled block sets one. */
static void fill_line_bg(Lay *L){
    if (!L->cur_bg_on || L->bg_line==L->cy) return;
    int sy=L->y0+L->cy-L->scroll;
    L->bg_line=L->cy;
    if (sy>=L->y0+L->vh || sy+L->line_h<=L->y0) return;
    int top=sy-1; if (top<L->y0) top=L->y0;
    int bot=sy+L->line_h; if (bot>L->y0+L->vh) bot=L->y0+L->vh;
    if (bot>top) gfx_fill(L->s, L->x0-4, top, L->cw+8, bot-top, L->cur_bg);
}

/* Nearest-neighbour blit of src scaled into (dx,dy,dw,dh), clipped to the content
 * viewport [cx0,cx1) x [cy0,cy1) so an image can't bleed into the toolbar/margins. */
static void img_draw(gfx_surface *s,int dx,int dy,int dw,int dh,const gfx_surface *src,
                     int cy0,int cy1,int cx0,int cx1){
    if (dw<=0||dh<=0||!src->px||src->w<=0||src->h<=0) return;
    if (cy0<0) cy0=0;
    if (cy1>s->h) cy1=s->h;
    if (cx0<0) cx0=0;
    if (cx1>s->w) cx1=s->w;
    for (int yy=0; yy<dh; yy++){
        int ddy=dy+yy; if (ddy<cy0||ddy>=cy1) continue;
        int ssy=yy*src->h/dh; const gfx_u32 *srow=&src->px[(unsigned)ssy*src->w];
        gfx_u32 *drow=&s->px[(unsigned)ddy*s->w];
        for (int xx=0; xx<dw; xx++){ int ddx=dx+xx; if (ddx<cx0||ddx>=cx1) continue; drow[ddx]=srow[(unsigned)xx*src->w/dw]; }
    }
}
/* Find "key: <N>px" inside a style attribute (e.g. width/height); 0 if absent. */
static int find_px(const char *s,const char *key){
    int kl=sl(key);
    for (const char *p=s; *p; p++){
        if (p!=s && p[-1]!=';' && p[-1]!=' ') continue;
        int i=0; while(i<kl && lc(p[i])==lc(key[i])) i++;
        if (i!=kl) continue;
        const char *q=p+kl; while(*q==' ')q++; if(*q!=':')continue; q++;
        int v=css_px(q,sl(q)); if(v>0) return v;
    }
    return 0;
}

static void emit_word(Lay *L,const char *w,int n){
    if (n<=0) return;
    int sc=L->cur_scale, gw=GLYPH_W*sc, ww=n*gw;
    if (L->need_sp && L->cx>L->indent) L->cx += SPACE_W*sc;
    L->need_sp = 0;
    if (L->cx>L->indent && L->cx+ww > L->cw) newline(L,0);
    if (gw+2 > L->line_h) L->line_h = gw+2;
    fill_line_bg(L);
    int sx = L->x0+L->cx, sy = L->y0+L->cy-L->scroll;
    if (sy >= L->y0 && sy+gw <= L->y0+L->vh) {
        gfx_u32 col = L->link ? COL_LINK : (L->cur_fg_on ? L->cur_fg : (L->cur_bold?COL_HEAD:COL_TEXT));
        draw_chars(L->s, sx, sy, w, n, col, L->cur_bold, L->x0+L->cw, sc);
        if (L->link) {
            int uw = ww; if (sx+uw > L->x0+L->cw) uw = L->x0+L->cw-sx;
            if (uw>0) gfx_fill(L->s, sx, sy+gw+1, uw, 1, COL_LINK);
        }
    }
    if (L->link) add_link(L, sx, L->cy, ww, L->line_h);   /* doc-y; click re-derives screen-y */
    L->cx += ww;
}

static void emit_img(Lay *L,const char *src,const char *alt,int reqw,int reqh){
    if (L->need_sp && L->cx>L->indent) { L->cx += SPACE_W; L->need_sp=0; }
    gfx_surface *im = img_find(src);
    if (im && im->px) {
        int iw=im->w, ih=im->h;
        if (reqw>0 && reqh>0) { iw=reqw; ih=reqh; }                 /* explicit w+h     */
        else if (reqw>0) { ih=im->h*reqw/im->w; iw=reqw; }          /* width, keep ratio */
        else if (reqh>0) { iw=im->w*reqh/im->h; ih=reqh; }          /* height, keep ratio*/
        if (iw > L->cw) { ih = ih*L->cw/iw; iw = L->cw; if(ih<1)ih=1; }
        if (L->cx>L->indent && L->cx+iw > L->cw) newline(L,0);
        if (ih+2 > L->line_h) L->line_h = ih+2;
        fill_line_bg(L);
        int sx=L->x0+L->cx, sy=L->y0+L->cy-L->scroll;
        img_draw(L->s, sx, sy, iw, ih, im, L->y0, L->y0+L->vh, L->x0, L->x0+L->cw);
        L->cx += iw+4;
    } else {
        int an=sl(alt); if(an>22)an=22;
        int bw=(an?an*GLYPH_W:48)+8, bh=18;
        if (L->cx>L->indent && L->cx+bw > L->cw) newline(L,0);
        if (bh+2 > L->line_h) L->line_h = bh+2;
        fill_line_bg(L);
        int sx=L->x0+L->cx, sy=L->y0+L->cy-L->scroll;
        if (sy>=L->y0 && sy+bh<=L->y0+L->vh) {
            gfx_outline(L->s,sx,sy,bw,bh,COL_RULE);
            if (an) draw_chars(L->s,sx+4,sy+5,alt,an,COL_MUTE,0,L->x0+L->cw,1);
        }
        L->cx += bw+4;
    }
}

/* dispatch one tag (open or close) -- block spacing, lists, rules, images.
 * Colour / weight / heading size come from the CSS + UA style stack (lay_push). */
static void handle_tag(Lay *L,const char *name,int close,const char *a,const char *e){
    if (seqi(name,"p"))  { block(L,6); return; }
    if (seqi(name,"br")) { if(L->cx>L->indent)newline(L,0); else L->cy+=L->line_h; return; }
    if (seqi(name,"a")) {
        if (close) { L->link=0; L->href=0; }
        else {
            char href[URLCAP];
            if (attr_get(a,e,"href",href,sizeof href) && href[0] && g_pool_used+sl(href)+1<POOL_MAX) {
                char *dst=g_pool+g_pool_used; scpy(dst,href,POOL_MAX-g_pool_used);
                g_pool_used += sl(dst)+1; L->href=dst; L->link=1;
            } else { L->link=1; L->href=0; }
        }
        return;
    }
    if (name[0]=='h' && name[1]>='1' && name[1]<='6' && !name[2]) {
        int big=(name[1]<='2'); block(L, close ? (big?8:6) : (big?10:8)); return;
    }
    if (seqi(name,"ul")||seqi(name,"ol")) {
        if (!close) { block(L,4); L->indent += 18; if(seqi(name,"ol")){L->ol=1;L->ord=0;} }
        else        { L->indent -= 18; if(L->indent<0)L->indent=0; L->ol=0; block(L,4); }
        return;
    }
    if (seqi(name,"li")) {
        if (!close) {
            if (L->cx>L->indent) newline(L,0);
            fill_line_bg(L);
            int sx=L->x0+L->indent-12, sy=L->y0+L->cy-L->scroll;
            if (L->ol) { L->ord++; char num[8]; char*o=pnum(num,(unsigned)L->ord); o=pcat(o,"."); *o=0; if(sy>=L->y0&&sy+GLYPH_W<=L->y0+L->vh) draw_chars(L->s,sx-4,sy,num,sl(num),COL_TEXT,0,L->x0+L->cw,1); }
            else if (sy>=L->y0 && sy+6<=L->y0+L->vh) gfx_fill(L->s,sx,sy+4,4,4,COL_TEXT);
        }
        return;
    }
    if (seqi(name,"hr")) {
        if (L->cx>L->indent) newline(L,0);
        L->cy += 4; int sy=L->y0+L->cy-L->scroll;
        if (sy>=L->y0 && sy<L->y0+L->vh) gfx_fill(L->s,L->x0,sy,L->cw,1,COL_RULE);
        L->cy += 6; return;
    }
    if (seqi(name,"img")) {
        char src[256], alt[64]; alt[0]=0;
        if (attr_get(a,e,"src",src,sizeof src)) {
            attr_get(a,e,"alt",alt,sizeof alt);
            char tmp[48], st[192]; int rw=0, rh=0;
            if (attr_get(a,e,"width",tmp,sizeof tmp))  rw=css_px(tmp,sl(tmp));
            if (attr_get(a,e,"height",tmp,sizeof tmp)) rh=css_px(tmp,sl(tmp));
            if (attr_get(a,e,"style",st,sizeof st)) { int v; if((v=find_px(st,"width")))rw=v; if((v=find_px(st,"height")))rh=v; }
            emit_img(L,src,alt,rw,rh);
        }
        return;
    }
    if (seqi(name,"input")) {
        char type[16]={0}; attr_get(a,e,"type",type,sizeof type);
        int ishidden=seqi(type,"hidden");
        int istext=(!type[0]||seqi(type,"text")||seqi(type,"search")||seqi(type,"url")||seqi(type,"email")||seqi(type,"password"));
        int issubmit=(seqi(type,"submit")||seqi(type,"button")||seqi(type,"image"));
        if (istext||ishidden) {
            int fi=L->field_n++;
            if (ishidden) return;                         /* counted, not drawn */
            char szs[8]; int bw=180; if (attr_get(a,e,"size",szs,sizeof szs)){ int s=css_px(szs,sl(szs)); if(s>0)bw=s*8+10; }
            if (bw>L->cw) bw=L->cw;
            int bh=18;
            if (L->cx>L->indent && L->cx+bw>L->cw) newline(L,0);
            if (bh+2>L->line_h) L->line_h=bh+2;
            fill_line_bg(L);
            int sx=L->x0+L->cx, sy=L->y0+L->cy-L->scroll, pw=seqi(type,"password");
            if (fi<MAX_FIELDS && sy>=L->y0 && sy+bh<=L->y0+L->vh) {
                int foc=(g_ff==fi);
                gfx_fill(L->s,sx,sy,bw,bh,RGB(0xff,0xff,0xff));
                gfx_outline(L->s,sx,sy,bw,bh,foc?COL_LINK:RGB(0x90,0x98,0xa4));
                const char *v=(fi<g_nfields)?g_fields[fi].value:"";
                char mask[260]; if (pw){ int n=sl(v); if(n>258)n=258; for(int k=0;k<n;k++)mask[k]='*'; mask[n]=0; v=mask; }
                int tw=gfx_text_w(v), vx=sx+4; if (tw>bw-8) vx=sx+bw-4-tw;
                draw_chars(L->s,vx,sy+5,v,sl(v),COL_TEXT,0,sx+bw-3,1);
                if (foc){ int cp=vx+tw; if(cp>sx+bw-4)cp=sx+bw-4; gfx_fill(L->s,cp,sy+3,1,bh-6,COL_TEXT); }
            }
            if (fi<MAX_FIELDS){ g_fields[fi].x=sx; g_fields[fi].dy=L->cy; g_fields[fi].w=bw; g_fields[fi].h=bh; g_fields[fi].shown=1; }
            L->cx+=bw+4;
            return;
        }
        if (issubmit) {
            char val[48]; if (!attr_get(a,e,"value",val,sizeof val)||!val[0]) scpy(val,"Search",sizeof val);
            int bw=gfx_text_w(val)+16, bh=18;
            if (L->cx>L->indent && L->cx+bw>L->cw) newline(L,0);
            if (bh+2>L->line_h) L->line_h=bh+2;
            fill_line_bg(L);
            int sx=L->x0+L->cx, sy=L->y0+L->cy-L->scroll;
            if (sy>=L->y0 && sy+bh<=L->y0+L->vh) {
                gfx_round(L->s,sx,sy,bw,bh,UI_COL_BTN,UI_COL_FIELD);
                draw_chars(L->s,sx+8,sy+5,val,sl(val),UI_COL_TEXT,0,sx+bw-4,1);
            }
            if (g_nsub<MAX_SUBS){ g_subs[g_nsub].x=sx; g_subs[g_nsub].dy=L->cy; g_subs[g_nsub].w=bw; g_subs[g_nsub].h=bh; g_nsub++; }
            L->cx+=bw+4;
            return;
        }
        return;                                           /* checkbox/radio/etc */
    }
    if (seqi(name,"pre")) { L->pre = !close; block(L,4); return; }
    if (seqi(name,"blockquote")) { if(!close){block(L,4);L->indent+=18;} else {L->indent-=18;if(L->indent<0)L->indent=0;block(L,4);} return; }
    if (seqi(name,"tr"))  { block(L,2); return; }
    if (seqi(name,"td")||seqi(name,"th")) { L->need_sp=1; L->cx += SPACE_W; return; }
    if (seqi(name,"div")||seqi(name,"section")||seqi(name,"article")||seqi(name,"header")||
        seqi(name,"footer")||seqi(name,"nav")||seqi(name,"main")||seqi(name,"table")||
        seqi(name,"figure")||seqi(name,"figcaption")||seqi(name,"form")||seqi(name,"dl")||
        seqi(name,"dd")||seqi(name,"dt")) { block(L,2); return; }
    /* span, i, em, font, small, code, ... inline/no-op (the style stack still applies) */
}

/* Walk the page and render into the surface; returns total document height. */
static int render_page(gfx_surface *s,int scroll){
    Lay L;
    L.s=s; L.y0=TOOLBAR_H;
    L.vh = s->h - TOOLBAR_H; if (L.vh<1) L.vh=1;
    /* centre a fixed-width card (div.main_page style) on the body background */
    int avail = s->w - 2*MARGIN - SBW; if (avail<48) avail=48;
    int cardx=MARGIN, cardw=avail;
    if (g_has_card && g_card_w>0 && g_card_w<avail) { cardw=g_card_w; cardx=MARGIN+(avail-cardw)/2; }
    gfx_fill(s, 0, L.y0, s->w, L.vh, g_has_body_bg?g_body_bg:PAGE_BG);
    if (g_has_card) {
        gfx_fill(s, cardx, L.y0, cardw, L.vh, g_card_bg);
        gfx_fill(s, cardx-2, L.y0, 2, L.vh, RGB(0x21,0x27,0x38));
        gfx_fill(s, cardx+cardw, L.y0, 2, L.vh, RGB(0x21,0x27,0x38));
    }
    L.x0 = cardx + (g_has_card?8:0);
    L.cw = cardw - (g_has_card?16:0); if (L.cw<48) L.cw=48;
    L.cx=0; L.cy=6; L.line_h=LINE_BASE; L.indent=0; L.scroll=scroll;
    L.need_sp=0; L.link=0; L.pre=0; L.ol=0; L.ord=0; L.href=0;
    L.sd=0; L.bg_line=-1;
    L.cur_fg=COL_TEXT; L.cur_fg_on=0; L.cur_bg=PAGE_BG; L.cur_bg_on=0; L.cur_bold=0; L.cur_scale=1;
    g_nlink=0; g_pool_used=0; g_nsub=0; L.field_n=0;

    const char *p=g_html, *end=g_html+g_html_len;
    char word[256]; int wl=0;
    int skip=0; char skipname[12]={0};

    while (p<end) {
        char c=*p;
        if (skip) {
            if (c=='<' && p[1]=='/') {
                const char *q=p+2; int i=0; char nm[12];
                while (q<end && i<11 && ((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z'))) nm[i++]=lc(*q++);
                nm[i]=0;
                if (seqi(nm,skipname)) { while(q<end&&*q!='>')q++; p=(q<end)?q+1:end; skip=0; continue; }
            }
            p++; continue;
        }
        if (c=='<') {
            if (wl) { emit_word(&L,word,wl); wl=0; }
            if (p[1]=='!') {
                if (p[2]=='-'&&p[3]=='-') { p+=4; while(p+2<end && !(p[0]=='-'&&p[1]=='-'&&p[2]=='>')) p++; p=(p+3<=end)?p+3:end; }
                else { while(p<end&&*p!='>')p++; if(p<end)p++; }
                continue;
            }
            const char *q=p+1; int close=0; if(*q=='/'){close=1;q++;}
            char name[16]; int nl=0;
            while (q<end && nl<15 && ((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9'))) name[nl++]=lc(*q++);
            name[nl]=0;
            const char *attr=q; while(q<end && *q!='>') q++; const char *tend=q; if(q<end)q++;
            if (nl) {
                int st_skip = (seqi(name,"script")||seqi(name,"style")||seqi(name,"title"));
                if (!close && !is_void(name) && !st_skip) {
                    if (autocloses(name)) {
                        if (seqi(name,"tr")) { while(L.sd>0 && (seqi(L.st[L.sd-1].tag,"td")||seqi(L.st[L.sd-1].tag,"th")||seqi(L.st[L.sd-1].tag,"tr"))) L.sd--; lay_recompute(&L); }
                        else if (L.sd>0 && seqi(L.st[L.sd-1].tag,name)) lay_pop(&L,name);
                    }
                    lay_push(&L,name,attr,tend);
                }
                handle_tag(&L,name,close,attr,tend);
                if (close && !is_void(name) && !st_skip) lay_pop(&L,name);
                if (!close && (st_skip || seqi(name,"head"))) {
                    if (!seqi(name,"head")) { skip=1; scpy(skipname,name,sizeof skipname); }
                }
            }
            p=q; continue;
        }
        if (L.pre) {
            if (c=='\n') { if(wl){emit_word(&L,word,wl);wl=0;} newline(&L,0); }
            else if (c=='\t') { if(wl){emit_word(&L,word,wl);wl=0;} L.cx+=4*SPACE_W; }
            else if (c==' ')  { if(wl){emit_word(&L,word,wl);wl=0;} L.cx+=SPACE_W; }
            else if (c=='&')  { char eb[8]; int en=entity(&p,end,eb); for(int i=0;i<en&&wl<255;i++)word[wl++]=eb[i]; continue; }
            else { if(wl<255)word[wl++]=c; }
            p++; continue;
        }
        if (c==' '||c=='\n'||c=='\t'||c=='\r') { if(wl){emit_word(&L,word,wl);wl=0;} L.need_sp=1; p++; continue; }
        if (c=='&') { char eb[8]; int en=entity(&p,end,eb); for(int i=0;i<en&&wl<255;i++)word[wl++]=eb[i]; continue; }
        if (wl<255) word[wl++]=c;
        p++;
    }
    if (wl) emit_word(&L,word,wl);
    return L.cy + L.line_h + 8;
}

/* ---- page loading -------------------------------------------------------- */
static void set_html(const char *lit){
    int n=sl(lit); if(n>HTML_MAX-1)n=HTML_MAX-1;
    for(int i=0;i<n;i++) g_html[i]=lit[i];
    g_html[n]=0; g_html_len=n;
}

static void error_page(const char *url,const char *why){
    char *o=g_html;
    o=pcat(o,"<h1>Cannot display page</h1><p>");
    o=pcat(o,why);
    o=pcat(o,"</p><p style>"); o=pcat(o,url); o=pcat(o,"</p>");
    *o=0; g_html_len=(int)(o-g_html);
    g_title[0]=0;
}

/* Fetch `tgt` into g_html.  Returns 0 ok (sets a status), -1 on failure (sets
 * g_status to the reason). */
static int get_page(const char *tgt){
    if (starts_ci(tgt,"https://")) {
        scpy(g_status,"HTTPS is moving to userspace — almost there",sizeof g_status); return -1;
    }
    const char *fpath;
    if (has_scheme(tgt)) {
        int rc = sys_wget(tgt, "/tmp/mxweb.page");
        if (rc < 0) {
            char *o;
            if (rc < -1) {
                int st = -rc;
                if (st>=300 && st<400) { o=pcat(g_status,"Redirected (HTTP "); o=pnum(o,(unsigned)st); o=pcat(o,") — usually to HTTPS, which isn't supported yet"); }
                else { o=pcat(g_status,"HTTP "); o=pnum(o,(unsigned)st); o=pcat(o," from the server"); }
            } else o=pcat(g_status,"Could not connect (DNS / no route / refused)");
            *o=0; return -1;
        }
        fpath = "/tmp/mxweb.page";
    } else {
        fpath = tgt;
        if (tgt[0]=='f'&&tgt[1]=='i'&&tgt[2]=='l'&&tgt[3]=='e') { const char*s=tgt; while(*s&&*s!='/')s++; while(*s=='/')s++; fpath=s-1; }
    }
    int fl=0; unsigned char *fb = read_file(fpath, &fl);
    if (!fb) { scpy(g_status,"Could not read the page",sizeof g_status); return -1; }
    if (fl>HTML_MAX-1) fl=HTML_MAX-1;
    for (int i=0;i<fl;i++) g_html[i]=(char)fb[i];
    g_html[fl]=0; g_html_len=fl;
    sys_munmap(fb,(unsigned long)fl+1);
    char *o=pcat(g_status,"Loaded "); o=pnum(o,(unsigned)g_html_len); o=pcat(o," bytes"); *o=0;
    return 0;
}

static void hist_push(const char *u){
    if (g_hist_i>=0 && seqi(g_hist[g_hist_i],u)) return;       /* reload, no dup */
    g_hist_n = g_hist_i+1;                                      /* drop forward */
    if (g_hist_n>=HIST_MAX) {                                   /* shift out oldest */
        for (int i=1;i<HIST_MAX;i++) scpy(g_hist[i-1],g_hist[i],URLCAP);
        g_hist_n=HIST_MAX-1; g_hist_i--;
    }
    scpy(g_hist[g_hist_n],u,URLCAP); g_hist_i=g_hist_n; g_hist_n++;
}

/* Pre-scan the page for <form> + text/hidden <input>s, capturing the action +
 * each field's name and initial value (once, on navigate). */
static int alnum_c(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'); }
static void scan_forms(void){
    g_nfields=0; g_ff=-1; g_form_action[0]=0;
    const char *p=g_html, *end=g_html+g_html_len;
    while (p<end && g_nfields<MAX_FIELDS) {
        if (*p=='<') {
            const char *q=p+1; if (*q=='/') { p++; continue; }
            if (lc(q[0])=='f'&&lc(q[1])=='o'&&lc(q[2])=='r'&&lc(q[3])=='m'&&!alnum_c(q[4])) {
                const char *a=q+4,*e=a; while(e<end&&*e!='>')e++;
                if (!g_form_action[0]) attr_get(a,e,"action",g_form_action,sizeof g_form_action);
                p=(e<end)?e+1:end; continue;
            }
            if (lc(q[0])=='i'&&lc(q[1])=='n'&&lc(q[2])=='p'&&lc(q[3])=='u'&&lc(q[4])=='t'&&!alnum_c(q[5])) {
                const char *a=q+5,*e=a; while(e<end&&*e!='>')e++;
                char type[16]={0}; attr_get(a,e,"type",type,sizeof type);
                if (!type[0]||seqi(type,"text")||seqi(type,"search")||seqi(type,"url")||
                    seqi(type,"email")||seqi(type,"password")||seqi(type,"hidden")) {
                    formfield *f=&g_fields[g_nfields++];
                    f->name[0]=f->value[0]=0; f->x=f->dy=f->w=f->h=0; f->shown=0;
                    attr_get(a,e,"name",f->name,sizeof f->name);
                    attr_get(a,e,"value",f->value,sizeof f->value);
                }
                p=(e<end)?e+1:end; continue;
            }
        }
        p++;
    }
}

static void navigate(const char *input,int push){
    char tgt[URLCAP];
    /* about:* pages (home / welcome) render from the built-in string -- no net */
    if (starts_ci(input,"about:")) {
        scpy(tgt,input,sizeof tgt);
        scpy(g_urlbar,tgt,sizeof g_urlbar);
        reset_images(); css_reset(); g_nfields=0; g_ff=-1;
        set_html(WELCOME);
        scpy(g_title,"mxweb",sizeof g_title);
        scpy(g_status,"Home",sizeof g_status);
        scpy(g_cur_url,tgt,sizeof g_cur_url);
        if (push) hist_push(tgt);
        g_scroll=0; g_drag=0;
        return;
    }
    url_normalise(input,tgt);
    scpy(g_urlbar,tgt,sizeof g_urlbar);
    g_status[0]=0;
    reset_images();
    int ok = (get_page(tgt)==0);
    scpy(g_cur_url,tgt,sizeof g_cur_url);
    if (push) hist_push(tgt);
    if (ok) { scan_title(); scan_css(); scan_images(); scan_forms(); }
    else    { css_reset(); g_nfields=0; g_ff=-1; error_page(tgt,g_status); }
    g_scroll=0; g_drag=0;
}

/* Submit the form: resolve the action, append name=value pairs as a query
 * string (urlencoded), and navigate.  GET-style; POST falls back to GET. */
static char *urlenc(char *o, const char *s){
    static const char H[]="0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c=(unsigned char)*s;
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') *o++=(char)c;
        else if (c==' ') *o++='+';
        else { *o++='%'; *o++=H[c>>4]; *o++=H[c&15]; }
    }
    return o;
}
static void form_submit(void){
    char act[URLCAP];
    if (g_form_action[0]) url_resolve(g_form_action, act, sizeof act);
    else scpy(act, g_cur_url, sizeof act);
    int has_q=0; for (const char *z=act; *z; z++) if (*z=='?') has_q=1;
    char url[URLCAP]; char *o=url; const char *lim=url+URLCAP-16;
    for (const char *z=act; *z && o<lim; ) *o++=*z++;
    char sep = has_q?'&':'?';
    for (int i=0;i<g_nfields && o<lim;i++) {
        if (!g_fields[i].name[0]) continue;
        *o++=sep; sep='&';
        o=urlenc(o, g_fields[i].name); *o++='=';
        o=urlenc(o, g_fields[i].value);
    }
    *o=0;
    navigate(url,1);
}

/* ---- scrollbar ----------------------------------------------------------- */
static void draw_scrollbar(gfx_surface *s,int cy,int vh){
    int sbx = s->w - SBW;
    gfx_fill(s, sbx, cy, SBW, vh, RGB(0x20,0x24,0x2c));
    int maxsc = g_content_h - vh; if (maxsc<1) return;
    int th = vh*vh/g_content_h; if (th<24) th=24; if (th>vh) th=vh;
    int ty = cy + (vh-th)*g_scroll/maxsc;
    gfx_fill(s, sbx+2, ty, SBW-4, th, RGB(0x55,0x60,0x72));
}

int main(int argc,char **argv){
    mx_conn c;
    if (mx_connect(&c,argc,argv,720,520,MX_F_RESIZABLE)!=0) return 1;

    g_html = xmap(HTML_MAX);
    g_pool = xmap(POOL_MAX);
    if (!g_html || !g_pool) { mx_close(&c); return 1; }

    /* a positional argv (skipping "-makx <pid>") is a URL or a local .html path */
    const char *target=0;
    for (int i=1;i<argc;i++) {
        if (argv[i][0]=='-') { if(seqi(argv[i],"-makx")) i++; continue; }
        target=argv[i]; break;
    }
    if (target) navigate(target,1);
    else navigate("about:start",1);

    ui_ctx u; for (unsigned i=0;i<sizeof u/sizeof(int);i++) ((int*)&u)[i]=0;
    int first=1, lmx=-1, lmy=-1, lfocus=-1, lkey=-2;

    while (!c.closed) {
        mx_pump(&c);
        int key = mx_key(&c);
        int changed = first || key>=0 || c.mpressed || c.mreleased || c.mdown ||
                      c.mx!=lmx || c.my!=lmy || c.focused!=lfocus || c.resized || key!=lkey;
        lmx=c.mx; lmy=c.my; lfocus=c.focused; lkey=key; first=0;
        if (!changed) { sys_yield(); continue; }

        gfx_surface *s=&c.surf;
        int cy0=TOOLBAR_H, vh=s->h-TOOLBAR_H; if(vh<1)vh=1;

        /* chrome */
        gfx_fill(s,0,0,s->w,TOOLBAR_H,UI_COL_FIELD_FC);   /* content area is cleared by render_page */

        /* route typed keys to a focused in-page form field (URL bar goes inert) */
        int uikey = key;
        if (g_ff>=0 && g_ff<g_nfields) {
            uikey = -1;
            if (key=='\n') form_submit();
            else if (key==8 || key==127) { char *v=g_fields[g_ff].value; int n=sl(v); if(n>0) v[n-1]=0; }
            else if (key>=32 && key<127) { char *v=g_fields[g_ff].value; int n=sl(v); if(n<255){ v[n]=(char)key; v[n+1]=0; } }
        }
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,(uikey=='\t')?-1:uikey);
        int back_c = ui_button(&u,s,6,6,46,20,"Back");
        int fwd_c  = ui_button(&u,s,54,6,46,20,"Fwd");
        int home_c = ui_button(&u,s,102,6,52,20,"Home");
        int bx=158, bw=s->w-12-bx-44; if(bw<60)bw=60;
        ui_textbox(&u,s,bx,6,bw,20,g_urlbar,(int)sizeof g_urlbar);
        int go_c   = ui_button(&u,s,bx+bw+4,6,40,20,"Go");

        /* status strip: page title (or url) left, status right */
        ui_label(&u,s,8,32,g_title[0]?g_title:g_cur_url,COL_MUTE);
        int stw=gfx_text_w(g_status); ui_label(&u,s,s->w-SBW-stw-6,32,g_status,COL_MUTE);

        /* keyboard scroll (only when no in-page field owns the key) */
        if (g_ff<0 && key==0x80) g_scroll -= 48;            /* up   */
        else if (g_ff<0 && key==0x81) g_scroll += 48;       /* down */

        /* scrollbar interaction (uses last frame's content height) */
        int sbx=s->w-SBW, maxsc=g_content_h-vh; if(maxsc<0)maxsc=0;
        if (g_content_h>vh) {
            int th=vh*vh/g_content_h; if(th<24)th=24; if(th>vh)th=vh;
            int ty=cy0+(maxsc?(vh-th)*g_scroll/maxsc:0);
            if (c.mpressed && c.mx>=sbx && c.my>=cy0 && c.my<cy0+vh) {
                if (c.my>=ty && c.my<ty+th) { g_drag=1; g_drag_off=c.my-ty; }
                else g_scroll += (c.my<ty? -vh : vh);     /* page toward click */
            }
            if (g_drag && c.mdown) { int nt=c.my-g_drag_off-cy0; g_scroll = maxsc?nt*maxsc/(vh-th):0; }
        }
        if (!c.mdown) g_drag=0;
        if (g_scroll>maxsc) g_scroll=maxsc;
        if (g_scroll<0) g_scroll=0;

        /* render the document */
        g_content_h = render_page(s,g_scroll);
        maxsc = g_content_h-vh; if(maxsc<0)maxsc=0;
        if (g_scroll>maxsc){ g_scroll=maxsc; g_content_h=render_page(s,g_scroll); }
        draw_scrollbar(s,cy0,vh);

        /* content clicks: submit buttons, then form fields, then links (not scrollbar) */
        if (c.mpressed && !g_drag && c.mx<sbx && c.my>=cy0) {
            int handled=0;
            for (int i=0;i<g_nsub && !handled;i++) {
                int ry=cy0+g_subs[i].dy-g_scroll;
                if (c.mx>=g_subs[i].x && c.mx<g_subs[i].x+g_subs[i].w && c.my>=ry && c.my<ry+g_subs[i].h) { form_submit(); handled=1; }
            }
            for (int i=0;i<g_nfields && !handled;i++) {
                if (!g_fields[i].shown) continue;
                int ry=cy0+g_fields[i].dy-g_scroll;
                if (c.mx>=g_fields[i].x && c.mx<g_fields[i].x+g_fields[i].w && c.my>=ry && c.my<ry+g_fields[i].h) { g_ff=i; handled=1; }
            }
            if (!handled) {
                g_ff=-1;
                for (int i=0;i<g_nlink;i++) {
                    int ly=cy0+g_links[i].y-g_scroll;          /* doc-y -> screen-y */
                    if (c.mx>=g_links[i].x && c.mx<g_links[i].x+g_links[i].w &&
                        c.my>=ly && c.my<ly+g_links[i].h && g_links[i].href) {
                        char res[URLCAP]; url_resolve(g_links[i].href,res,sizeof res);
                        navigate(res,1); break;
                    }
                }
            }
        }
        if (c.mpressed && c.my<cy0) g_ff=-1;   /* clicked the toolbar -> defocus fields */

        /* navigation buttons (Reload = press Enter in the URL bar) */
        if      (back_c && g_hist_i>0)              { g_hist_i--; navigate(g_hist[g_hist_i],0); }
        else if (fwd_c  && g_hist_i<g_hist_n-1)     { g_hist_i++; navigate(g_hist[g_hist_i],0); }
        else if (home_c)                            navigate("about:start",1);
        else if (go_c && g_urlbar[0])               navigate(g_urlbar,1);
        else if (key=='\n' && g_urlbar[0])          navigate(g_urlbar,1);

        mx_present(&c);
        sys_yield();
    }
    reset_images();
    mx_close(&c);
    return 0;
}
