/*
 * tar.elf -- a small userspace tar(1): USTAR create / extract / list, with
 * optional gzip (z).  100% userspace -- just open/read/write/close + the shared
 * DEFLATE decompressor (inflate.h).  gzip create uses stored-DEFLATE blocks
 * (valid .gz, no actual compression yet); gzip extract inflates normally.
 *
 *   tar c[z]vf out.tar[.gz] PATHS...    create
 *   tar x[z]vf arc.tar[.gz]            extract
 *   tar t[z]vf arc.tar[.gz]            list
 */
#include "syscall.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

#define BLK 512

static void out(const char *s){ unsigned n=0; while(s[n])n++; sys_write(1,s,n); }
static void err(const char *s){ unsigned n=0; while(s[n])n++; sys_write(2,s,n); }

/* ---- CRC32 (gzip) -------------------------------------------------------- */
static unsigned crc_tab[256]; static int crc_ready;
static void crc_init(void){ for(unsigned i=0;i<256;i++){ unsigned c=i; for(int k=0;k<8;k++) c=(c&1)?(0xEDB88320u^(c>>1)):(c>>1); crc_tab[i]=c; } crc_ready=1; }
static unsigned crc32(const unsigned char *d, unsigned n){ if(!crc_ready)crc_init(); unsigned c=0xFFFFFFFFu; for(unsigned i=0;i<n;i++) c=crc_tab[(c^d[i])&0xFF]^(c>>8); return c^0xFFFFFFFFu; }

/* ---- growable byte buffer ------------------------------------------------ */
typedef struct { unsigned char *p; unsigned len, cap; } buf;
static int bput(buf *b, const void *d, unsigned n){
    if(b->len+n > b->cap){ unsigned nc=b->cap?b->cap:8192; while(nc<b->len+n)nc<<=1; unsigned char *q=realloc(b->p,nc); if(!q)return -1; b->p=q; b->cap=nc; }
    if(d) memcpy(b->p+b->len,d,n); else memset(b->p+b->len,0,n);
    b->len+=n; return 0;
}

/* ---- octal helpers ------------------------------------------------------- */
static void wr_octal(char *f, int w, unsigned long v){ /* w-1 digits + NUL */
    f[w-1]=0; for(int i=w-2;i>=0;i--){ f[i]=(char)('0'+(v&7)); v>>=3; }
}
static unsigned long rd_octal(const char *f, int w){ unsigned long v=0; int i=0; while(i<w&&(f[i]==' '||f[i]==0))i++; for(;i<w&&f[i]>='0'&&f[i]<='7';i++) v=(v<<3)|(unsigned long)(f[i]-'0'); return v; }

/* ---- USTAR header (512 bytes) ------------------------------------------- */
static void hdr_make(unsigned char *h, const char *name, unsigned long size, int isdir){
    memset(h,0,BLK);
    unsigned n=0; while(name[n]&&n<99){ h[n]=(unsigned char)name[n]; n++; }
    wr_octal((char*)h+100,8,isdir?0755:0644);   /* mode */
    wr_octal((char*)h+108,8,0); wr_octal((char*)h+116,8,0);  /* uid/gid */
    wr_octal((char*)h+124,12,size);
    wr_octal((char*)h+136,12,0);                /* mtime */
    h[156]=isdir?'5':'0';                       /* typeflag */
    memcpy(h+257,"ustar",6); h[263]='0'; h[264]='0';
    for(int i=0;i<8;i++) h[148+i]=' ';          /* chksum field = spaces */
    unsigned sum=0; for(int i=0;i<BLK;i++) sum+=h[i];
    wr_octal((char*)h+148,7,sum); h[155]=' ';   /* 6 octal digits + NUL + space */
}

/* ---- create -------------------------------------------------------------- */
static buf g_ar;

static int add_path(const char *path){
    struct stat st;
    if(sys_stat(path,&st)!=0){ err("tar: cannot stat "); err(path); err("\n"); return -1; }
    int isdir = S_ISDIR(st.st_mode);
    unsigned char h[BLK];
    if(isdir){
        char dn[101]; unsigned i=0; while(path[i]&&i<99){dn[i]=path[i];i++;} if(i&&dn[i-1]!='/'&&i<99)dn[i++]='/'; dn[i]=0;
        hdr_make(h,dn,0,1); bput(&g_ar,h,BLK);
        for(unsigned idx=0;;idx++){
            struct dirent de;
            int r=sys_readdir(path,idx,&de); if(r<=0) break;
            if(de.d_name[0]=='.'&&(de.d_name[1]==0||(de.d_name[1]=='.'&&de.d_name[2]==0))) continue;
            char child[512]; unsigned c=0; for(;path[c]&&c<255;c++)child[c]=path[c]; if(c&&child[c-1]!='/')child[c++]='/'; for(unsigned k=0;de.d_name[k]&&c<511;k++)child[c++]=de.d_name[k]; child[c]=0;
            add_path(child);
        }
        return 0;
    }
    /* regular file: read all, append header + padded data */
    int fd=sys_open(path,O_RDONLY); if(fd<0){ err("tar: cannot open "); err(path); err("\n"); return -1; }
    buf fb={0,0,0}; char tmp[4096]; long n;
    while((n=sys_read(fd,tmp,sizeof tmp))>0) bput(&fb,tmp,(unsigned)n);
    sys_close(fd);
    hdr_make(h,path,fb.len,0); bput(&g_ar,h,BLK);
    bput(&g_ar,fb.p,fb.len);
    unsigned pad=(BLK-(fb.len%BLK))%BLK; if(pad) bput(&g_ar,0,pad);
    free(fb.p);
    return 0;
}

/* ---- extract / list ------------------------------------------------------ */
static int extract(const unsigned char *t, unsigned len, int verbose, int list_only){
    unsigned pos=0;
    while(pos+BLK<=len){
        const unsigned char *h=t+pos;
        int zero=1; for(int i=0;i<BLK;i++) if(h[i]){zero=0;break;}
        if(zero) break;                          /* end-of-archive */
        pos+=BLK;
        char name[101]; memcpy(name,h,100); name[100]=0;
        unsigned long size=rd_octal((const char*)h+124,12);
        int isdir = (h[156]=='5') || (name[0]&&name[strlen(name)-1]=='/');
        if(verbose||list_only){ out(name); out("\n"); }
        if(!list_only){
            if(isdir){ sys_mkdir(name,0755); }
            else {
                if(pos+size<=len) sys_write_file(name,t+pos,(unsigned)size);
            }
        }
        pos += (unsigned)((size+BLK-1)/BLK)*BLK;  /* skip padded data */
    }
    return 0;
}

/* ---- gzip ---------------------------------------------------------------- */
/* Wrap raw bytes as gzip using stored DEFLATE blocks (valid .gz, uncompressed). */
static int gz_wrap(buf *b, const unsigned char *d, unsigned len){
    unsigned char hd[10]={0x1f,0x8b,8,0,0,0,0,0,0,0xff};
    if(bput(b,hd,10)) return -1;
    unsigned off=0;
    do {
        unsigned chunk = len-off; if(chunk>65535) chunk=65535;
        unsigned char bh[5]; bh[0]=(off+chunk>=len)?1:0;          /* BFINAL, BTYPE=00 */
        bh[1]=(unsigned char)(chunk&0xff); bh[2]=(unsigned char)(chunk>>8);
        bh[3]=(unsigned char)(~chunk&0xff); bh[4]=(unsigned char)((~chunk>>8)&0xff);
        if(bput(b,bh,5)) return -1;
        if(chunk && bput(b,d+off,chunk)) return -1;
        off+=chunk;
    } while(off<len);
    unsigned crc=crc32(d,len);
    unsigned char tr[8]={(unsigned char)crc,(unsigned char)(crc>>8),(unsigned char)(crc>>16),(unsigned char)(crc>>24),
                         (unsigned char)len,(unsigned char)(len>>8),(unsigned char)(len>>16),(unsigned char)(len>>24)};
    return bput(b,tr,8);
}
/* Inflate a gzip stream into a fresh malloc'd buffer; *outlen set.  NULL on error. */
static unsigned char *gz_unwrap(const unsigned char *g, unsigned glen, unsigned *outlen){
    if(glen<18||g[0]!=0x1f||g[1]!=0x8b||g[2]!=8) return 0;
    unsigned flg=g[3], p=10;
    if(flg&4){ if(p+2>glen)return 0; unsigned xl=g[p]|((unsigned)g[p+1]<<8); p+=2+xl; }
    if(flg&8){ while(p<glen&&g[p])p++; p++; }      /* FNAME */
    if(flg&16){ while(p<glen&&g[p])p++; p++; }     /* FCOMMENT */
    if(flg&2) p+=2;                                /* FHCRC */
    if(p>=glen) return 0;
    unsigned isize = g[glen-4]|((unsigned)g[glen-3]<<8)|((unsigned)g[glen-2]<<16)|((unsigned)g[glen-1]<<24);
    unsigned char *o=malloc(isize?isize:1); if(!o) return 0;
    long got=inflate(g+p, glen-p-8, o, isize?isize:1);
    if(got<0){ free(o); return 0; }
    *outlen=(unsigned)got; return o;
}

/* ---- read a whole file --------------------------------------------------- */
static unsigned char *slurp(const char *path, unsigned *len){
    int fd=sys_open(path,O_RDONLY); if(fd<0) return 0;
    buf b={0,0,0}; char tmp[8192]; long n;
    while((n=sys_read(fd,tmp,sizeof tmp))>0) bput(&b,tmp,(unsigned)n);
    sys_close(fd); *len=b.len; return b.p;
}

int main(int argc, char **argv){
    int mode=0, verbose=0, gz=0, fi=-1;          /* mode: 'c'/'x'/'t' */
    int ai=1;
    if(ai<argc){
        const char *f=argv[ai]; if(f[0]=='-')f++;
        for(;*f;f++){ if(*f=='c'||*f=='x'||*f=='t')mode=*f; else if(*f=='v')verbose=1; else if(*f=='z')gz=1; else if(*f=='f')fi=1; }
        ai++;
    }
    if(!mode){ err("usage: tar c|x|t [z] v f <archive> [paths...]\n"); return 1; }
    const char *arch=0;
    if(fi>0){ if(ai<argc) arch=argv[ai++]; else { err("tar: f given but no archive\n"); return 1; } }
    if(!arch){ err("tar: stdin/stdout archives not supported; use f <file>\n"); return 1; }

    if(mode=='c'){
        for(int i=ai;i<argc;i++) add_path(argv[i]);
        bput(&g_ar,0,BLK*2);                      /* two zero blocks */
        int rc;
        if(gz){ buf z={0,0,0}; if(gz_wrap(&z,g_ar.p,g_ar.len)){ err("tar: gzip failed\n"); return 1; } rc=sys_write_file(arch,z.p,z.len); free(z.p); }
        else rc=sys_write_file(arch,g_ar.p,g_ar.len);
        if(rc!=0){ err("tar: cannot write "); err(arch); err("\n"); return 1; }
        if(verbose){ out("tar: wrote "); out(arch); out("\n"); }
        return 0;
    }

    /* x / t : read the archive (gunzip if needed), then walk it */
    unsigned alen=0; unsigned char *a=slurp(arch,&alen);
    if(!a){ err("tar: cannot read "); err(arch); err("\n"); return 1; }
    unsigned char *tarbuf=a; unsigned tarlen=alen; unsigned char *freed=0;
    if(gz || (alen>2&&a[0]==0x1f&&a[1]==0x8b)){
        unsigned ul=0; unsigned char *u=gz_unwrap(a,alen,&ul);
        if(!u){ err("tar: bad gzip\n"); free(a); return 1; }
        tarbuf=u; tarlen=ul; freed=u;
    }
    extract(tarbuf,tarlen,verbose,mode=='t');
    free(a); if(freed) free(freed);
    return 0;
}
