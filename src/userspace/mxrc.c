/*
 * mxrc.c -- the per-user GUI profile, ~/.mxrc (see mxrc.h).
 */
#include "syscall.h"
#include "mxrc.h"

static int  m_slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static void m_cpy(char *d, const char *s, int cap){ int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }
static char *m_cat(char *p, const char *s){ while(*s) *p++=*s++; return p; }

void mxrc_home(const char *suffix, char *out, int cap)
{
    char u[64]={0}; sys_whoami(u, sizeof u);
    int isroot = (u[0]=='r'&&u[1]=='o'&&u[2]=='o'&&u[3]=='t'&&u[4]==0);
    int n=0;
    if (!u[0] || isroot){ const char *r="/root"; while (*r && n<cap-1) out[n++]=*r++; }
    else { const char *pre="/home/"; while (*pre && n<cap-1) out[n++]=*pre++;
           for (int k=0; u[k] && n<cap-1; k++) out[n++]=u[k]; }
    for (const char *p=suffix; *p && n<cap-1; p++) out[n++]=*p;
    out[n]=0;
}

int mxrc_get_file(const char *rcname, const char *key, char *out, int cap)
{
    char path[96]; mxrc_home(rcname, path, sizeof path);
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[2048]; int n=0; long r;
    while (n < (int)sizeof buf-1 && (r=sys_read(fd, buf+n, (unsigned)((int)sizeof buf-1-n))) > 0) n += (int)r;
    sys_close(fd); buf[n]=0;
    int kl = m_slen(key), i=0;
    while (i < n){
        int s=i; while (i<n && buf[i]!='\n' && buf[i]!='\r') i++; buf[i]=0;
        char *line=buf+s; i++; while (i<n && (buf[i]=='\n'||buf[i]=='\r')) i++;
        int m=1; for (int k=0;k<kl;k++) if (line[k]!=key[k]){ m=0; break; }
        if (m && line[kl]=='='){ m_cpy(out, line+kl+1, cap); return 0; }
    }
    return -1;
}

int mxrc_get(const char *key, char *out, int cap)
{ return mxrc_get_file("/.mxrc", key, out, cap); }

int mxrc_get_int(const char *key, int def)
{
    char v[32];
    if (mxrc_get(key, v, sizeof v)!=0) return def;
    int n=0, neg=0; const char *p=v;
    if (*p=='-'){ neg=1; p++; }
    if (*p<'0'||*p>'9') return def;
    while (*p>='0'&&*p<='9'){ n=n*10+(*p-'0'); p++; }
    return neg?-n:n;
}

/* Read-modify-write: replace `key`'s line (or append it), keep every other. */
void mxrc_set_file(const char *rcname, const char *key, const char *val)
{
    char path[96]; mxrc_home(rcname, path, sizeof path);
    static char buf[2048]; int n=0;
    int fd = sys_open(path, O_RDONLY);
    if (fd >= 0){ long r; while (n<(int)sizeof buf-1 && (r=sys_read(fd, buf+n, (unsigned)((int)sizeof buf-1-n)))>0) n+=(int)r; sys_close(fd); }
    buf[n]=0;

    static char out[2300]; char *o=out; int kl=m_slen(key), found=0, i=0;
    while (i < n){
        int s=i; while (i<n && buf[i]!='\n' && buf[i]!='\r') i++;
        int linelen=i-s;
        while (i<n && (buf[i]=='\n'||buf[i]=='\r')) i++;
        int match = linelen>kl && buf[s+kl]=='=';
        if (match) for (int k=0;k<kl;k++) if (buf[s+k]!=key[k]){ match=0; break; }
        if (linelen==0) continue;
        if (match){ o=m_cat(o,key); *o++='='; o=m_cat(o,val); *o++='\n'; found=1; }
        else { for (int k=0; k<linelen && o<out+sizeof out-2; k++) *o++=buf[s+k]; *o++='\n'; }
    }
    if (!found){ o=m_cat(o,key); *o++='='; o=m_cat(o,val); *o++='\n'; }
    *o=0;

    fd = sys_open(path, O_WRONLY|O_CREAT|O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, out, (unsigned)(o-out));
    sys_close(fd);
}

void mxrc_set(const char *key, const char *val)
{ mxrc_set_file("/.mxrc", key, val); }

void mxrc_set_int(const char *key, int val)
{
    char v[16]; int n=0, neg = val<0;
    if (neg) val=-val;
    char t[12]; int ti=0;
    if (!val) t[ti++]='0';
    while (val){ t[ti++]=(char)('0'+val%10); val/=10; }
    if (neg) v[n++]='-';
    while (ti) v[n++]=t[--ti];
    v[n]=0;
    mxrc_set(key, v);
}
