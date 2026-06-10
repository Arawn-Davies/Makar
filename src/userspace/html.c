/*
 * html.c -- shared HTML/URL parser primitives (see html.h).  Lifted verbatim
 * from mxweb's tokeniser so both browsers decode entities, read attributes and
 * resolve URLs identically; mxweb's renderer and linx's text reflow are layered
 * on top of this common core.  Keeps its own tiny string helpers so it has no
 * dependency on either browser.
 */

#include "html.h"

#define HTML_URLCAP 1024

/* ---- private string helpers (freestanding) ------------------------------- */
static int  sl(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
static void scpy(char *d,const char *s,int max){ int i=0; if(max<=0)return; while(s&&s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0; }
static int  seqi(const char *a,const char *b){ int i=0; for(;;){ char x=lc(a[i]),y=lc(b[i]); if(x!=y)return 0; if(!x)return 1; i++; } }
static char *pcat(char *p,const char *s){ while(s&&*s)*p++=*s++; return p; }
static int  udec(const char *s){ int v=0; while(*s>='0'&&*s<='9') v=v*10+(*s++-'0'); return v; }
static int  uhex(const char *s){ int v=0; for(;;){ char c=lc(*s++); int d; if(c>='0'&&c<='9')d=c-'0'; else if(c>='a'&&c<='f')d=c-'a'+10; else break; v=v*16+d; } return v; }

/* ---- public API ---------------------------------------------------------- */

int html_has_scheme(const char *s){
    for(int i=0;s&&s[i]&&i<16;i++){ if(s[i]==':'&&s[i+1]=='/'&&s[i+2]=='/')return 1; if(s[i]=='/')return 0; }
    return 0;
}

int html_entity(const char **pp, const char *end, char *out){
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

int html_attr_get(const char *a, const char *e, const char *name, char *out, int cap){
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

void html_url_split(const char *u, char *scheme, char *host, char *path, int pathcap){
    scheme[0]=host[0]=0; path[0]=0;
    const char *p = u;
    int has = html_has_scheme(u);
    if (has) {
        int i=0; while(*p && *p!=':' && i<15) scheme[i++]=lc(*p++); scheme[i]=0;
        if (*p==':') p++;
        if (*p=='/') p++;
        if (*p=='/') p++;                                       /* skip "://" */
        i=0; while(*p && *p!='/' && i<255) host[i++]=*p++; host[i]=0;
    }
    if (*p=='/' ) scpy(path, p, pathcap);
    else if (!has) scpy(path, u, pathcap);     /* relative / local path */
    else { path[0]='/'; scpy(path+1, p, pathcap-1); }
    if (!path[0]) { path[0]='/'; path[1]=0; }
}

void html_url_resolve(const char *base, const char *ref, char *out, int cap){
    if (!ref || !ref[0]) { scpy(out, base?base:"", cap); return; }
    if (html_has_scheme(ref)) { scpy(out, ref, cap); return; }
    char scheme[16], host[256], path[HTML_URLCAP];
    html_url_split(base?base:"", scheme, host, path, sizeof path);
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

void html_url_normalise(const char *in, char *out, int cap){
    while (*in==' '||*in=='\t') in++;
    char tmp[HTML_URLCAP];
    if (in[0]=='/' || html_has_scheme(in)) scpy(tmp, in, sizeof tmp);
    else { char *o=pcat(tmp,"http://"); scpy(o, in, (int)(sizeof tmp-7)); }
    /* strip trailing whitespace */
    int n=sl(tmp); while(n>0 && (tmp[n-1]==' '||tmp[n-1]=='\n'||tmp[n-1]=='\r')) tmp[--n]=0;
    /* a bare http://host with no path -> add "/" */
    if (html_has_scheme(tmp)) {
        char sc[16],ho[256],pa[HTML_URLCAP]; html_url_split(tmp,sc,ho,pa,sizeof pa);
        char *o=pcat(out,sc); o=pcat(o,"://"); o=pcat(o,ho); o=pcat(o,pa); *o=0;
    } else scpy(out, tmp, cap);
}
