/*
 * mximg.elf -- an image viewer, as a makx client.  Opens an image (Open dialog
 * or a path argument), decodes it, and scales it to fit the window (aspect-
 * preserving, nearest-neighbour).  Decodes BMP (24/32-bpp uncompressed), GIF
 * (87a/89a first frame, LZW, interlace) and PNG (the shared from-scratch
 * inflate + all scanline filters, bit depths 1-16, colour types 0/2/3/4/6,
 * non-interlaced) and baseline JPEG.  Reuses the shared
 * gui_browser file dialog (like mxedit) so it's usable straight from its icon.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
#include "img_bmp.h"
#include "img_png.h"
#include "img_jpg.h"
#include "mxrc.h"
#include "makx.h"

#define RGB GFX_RGB
#define COL_BG   RGB(0x0c,0x0e,0x12)
#define COL_BAR  RGB(0x16,0x1b,0x24)
#define COL_TEXT RGB(0xd3,0xd7,0xcf)
#define COL_ERR  RGB(0xff,0x60,0x60)

#define IMG_MAXW 1600
#define IMG_MAXH 1200
#define FILE_CAP (8u*1024u*1024u)

static gfx_u32 *img_px;          /* decoded pixels (mmap, IMG_MAXW*IMG_MAXH) */
static unsigned char *fbuf;            /* file read buffer (mmap, FILE_CAP)        */
static int img_w, img_h;         /* current image size (0 = none)           */
static char msg[96] = "Open an image (BMP/GIF/PNG/JPEG).";
static char g_cur_path[256];     /* full path of the loaded image (for wallpaper) */

/* ---- gallery: thumbnails of ~/Pictures + the bundled /usr/share images ---- */
#define GAL_MAX  96
#define THUMB_W  112
#define THUMB_H  84
static char      gal_path[GAL_MAX][256];
static char      gal_name[GAL_MAX][40];
static int       gal_n = 0;
static gfx_u32  *gal_thumb;       /* mmap: GAL_MAX thumbnails, THUMB_W*THUMB_H each */
static unsigned char gal_ok[GAL_MAX];   /* 0 = undecoded, 1 = ok, 2 = failed */
static int       g_gallery = 0;
static int       gal_scroll = 0;

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static unsigned rd32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static int      rd16(const unsigned char *p){ return p[0]|(p[1]<<8); }

/* Decode an uncompressed 24/32-bpp BMP from fbuf[0..n) into img_px.  Thin
 * wrapper over the shared bmp_decode (img_bmp.c) that sets the status line. */
static int decode_bmp(unsigned n)
{
    if (bmp_decode(fbuf, n, img_px, IMG_MAXW, IMG_MAXH, &img_w, &img_h) != 0) {
        scpy(msg, "unsupported BMP (need 24/32-bpp uncompressed)", sizeof msg);
        return -1;
    }
    return 0;
}

/* Decode a baseline JPEG from fbuf[0..n) into img_px (shared jpg_decode). */
static int decode_jpg(unsigned n)
{
    if (jpg_decode(fbuf, n, img_px, IMG_MAXW, IMG_MAXH, &img_w, &img_h,
                   msg, sizeof msg) != 0)
        return -1;
    return 0;
}

/* Decode a PNG from fbuf[0..n) into img_px.  Thin wrapper over the shared
 * png_decode (img_png.c); it writes its own reason into msg on failure. */
static int decode_png(unsigned n)
{
    if (png_decode(fbuf, n, img_px, IMG_MAXW, IMG_MAXH, &img_w, &img_h,
                   msg, sizeof msg) != 0)
        return -1;
    return 0;
}

/* ---- GIF (87a/89a): first frame, LZW, global/local palette, interlace ---- */
static unsigned short g_pfx[4096];
static unsigned char  g_sfx[4096];
static unsigned char  g_stk[4096];
static unsigned char  g_pal[256][3];

static const unsigned char *g_dp,*g_dend; static int g_dsub;     /* sub-block byte source */
static int gbyte(void){
    if(g_dsub==0){ if(g_dp>=g_dend) return -1; g_dsub=*g_dp++; if(g_dsub==0) return -1; }
    if(g_dp>=g_dend) return -1;
    g_dsub--; return *g_dp++;
}
static unsigned g_acc; static int g_nb;
static int gcode(int w){
    while(g_nb<w){ int b=gbyte(); if(b<0) return -1; g_acc|=((unsigned)b)<<g_nb; g_nb+=8; }
    int c=(int)(g_acc&((1u<<w)-1)); g_acc>>=w; g_nb-=w; return c;
}

static int decode_gif(unsigned n)
{
    if(n<13 || fbuf[0]!='G'||fbuf[1]!='I'||fbuf[2]!='F'){ scpy(msg,"not a GIF file",sizeof msg); return -1; }
    int sw=rd16(fbuf+6), sh=rd16(fbuf+8);
    unsigned char packed=fbuf[10];
    unsigned p=13;
    int gct = packed&0x80, gctsz = 2<<(packed&7);
    if(gct){ for(int i=0;i<gctsz && p+3<=n;i++){ g_pal[i][0]=fbuf[p];g_pal[i][1]=fbuf[p+1];g_pal[i][2]=fbuf[p+2];p+=3; } }
    (void)sw;(void)sh;
    /* walk blocks to the first image descriptor (skip extensions). */
    while(p<n){
        unsigned char b=fbuf[p++];
        if(b==0x3B) break;                       /* trailer, no image */
        if(b==0x21){ p++; while(p<n){ int len=fbuf[p++]; if(!len)break; p+=len; } continue; } /* extension */
        if(b!=0x2C) continue;                    /* unknown */
        if(p+9>n){ scpy(msg,"truncated GIF",sizeof msg); return -1; }
        int iw=rd16(fbuf+p+4), ih=rd16(fbuf+p+6);
        unsigned char ip=fbuf[p+8]; p+=9;
        int interlace = ip&0x40;
        if(ip&0x80){ int lsz=2<<(ip&7); for(int i=0;i<lsz && p+3<=n;i++){ g_pal[i][0]=fbuf[p];g_pal[i][1]=fbuf[p+1];g_pal[i][2]=fbuf[p+2];p+=3; } }
        if(iw<1||ih<1||iw>IMG_MAXW||ih>IMG_MAXH){ scpy(msg,"image too large",sizeof msg); return -1; }
        if(p>=n){ scpy(msg,"truncated GIF",sizeof msg); return -1; }
        int mincode=fbuf[p++];
        if(mincode<1||mincode>8){ scpy(msg,"bad GIF LZW",sizeof msg); return -1; }
        g_dp=fbuf+p; g_dend=fbuf+n; g_dsub=0; g_acc=0; g_nb=0;
        int clear=1<<mincode, end=clear+1, csz=mincode+1, avail=end+1, oldc=-1;
        unsigned char first=0;
        int starts[4]={0,4,2,1}, steps[4]={8,8,4,2}, npass=interlace?4:1;
        int pass=0, gx=0, gy=interlace?0:0;
        for(;;){
            int code=gcode(csz);
            if(code<0||code==end) break;
            if(code==clear){ csz=mincode+1; avail=end+1; oldc=-1; continue; }
            int sp=0, c;
            if(oldc<0){ first=(unsigned char)code; c=code; }
            else { if(code<avail) c=code; else { g_stk[sp++]=first; c=oldc; } }
            while(c>=clear){ if(sp>=4096)break; g_stk[sp++]=g_sfx[c]; c=g_pfx[c]; }
            first=(unsigned char)c; g_stk[sp++]=(unsigned char)c;
            while(sp>0){
                unsigned char idx=g_stk[--sp];
                if(gy<ih){ img_px[(unsigned)gy*iw+gx]=RGB(g_pal[idx][0],g_pal[idx][1],g_pal[idx][2]); }
                if(++gx>=iw){ gx=0; gy+=steps[pass];
                    while(gy>=ih && pass+1<npass){ pass++; gy=starts[pass]; } }
            }
            if(oldc>=0 && avail<4096){ g_pfx[avail]=(unsigned short)oldc; g_sfx[avail]=first; avail++;
                if(avail==(1<<csz) && csz<12) csz++; }
            oldc=code;
        }
        img_w=iw; img_h=ih;
        return 0;
    }
    scpy(msg,"no image in GIF",sizeof msg);
    return -1;
}

static int decode_image(unsigned n)
{
    if(n>=2 && fbuf[0]=='B'&&fbuf[1]=='M') return decode_bmp(n);
    if(n>=3 && fbuf[0]=='G'&&fbuf[1]=='I'&&fbuf[2]=='F') return decode_gif(n);
    if(n>=8 && fbuf[0]==0x89&&fbuf[1]=='P'&&fbuf[2]=='N'&&fbuf[3]=='G') return decode_png(n);
    if(n>=2 && fbuf[0]==0xFF&&fbuf[1]==0xD8) return decode_jpg(n);
    scpy(msg,"unsupported format (BMP/GIF/PNG/JPEG)",sizeof msg);
    return -1;
}

static void load_image(const char *path)
{
    img_w = img_h = 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { scpy(msg,"cannot open file",sizeof msg); return; }
    long sz = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, 0);
    if (sz <= 0 || (unsigned long)sz > FILE_CAP) { sys_close(fd); scpy(msg,"file too large",sizeof msg); return; }
    unsigned got = 0;
    while (got < (unsigned)sz) {
        long r = sys_read(fd, fbuf+got, (unsigned)sz-got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    sys_close(fd);
    if (decode_image(got) == 0) {
        scpy(g_cur_path, path, sizeof g_cur_path);
        /* path basename into msg */
        const char *b = path; for (const char *p=path; *p; p++) if (*p=='/') b=p+1;
        scpy(msg, b, sizeof msg);
    }
}

static int g_wp_sid = -1;        /* our shared wallpaper surface (destroy on replace) */

/* Set the loaded image as the desktop wallpaper.  Two parts, mirroring a Linux
 * desktop: (1) persist the path to ~/.mxrc so the WM reloads it at next boot,
 * and (2) hand the WM the decoded pixels *now* as a shared surface (X11
 * root-pixmap style) so it applies instantly without a cross-process file read. */
static void set_wallpaper(mx_conn *c)
{
    if (!g_cur_path[0] || img_w < 1){ scpy(msg,"open an image first",sizeof msg); return; }

    /* (1) persistence: ~/.mxrc Wallpaper=<path> (preserves other keys) */
    mxrc_set("Wallpaper", g_cur_path);

    /* (2) live: copy the decoded pixels into a shared surface + hand off the id */
    int sid=sys_surface_create(img_w, img_h);
    if (sid>=0){
        gfx_u32 *base=(gfx_u32*)sys_surface_map(sid);
        if (base && base!=(gfx_u32*)MAP_FAILED){
            long npx=(long)img_w*img_h;
            for (long i=0;i<npx;i++) base[i]=img_px[i];
            sys_surface_unmap(sid);                  /* WM holds it via the sid */
            if (g_wp_sid>=0) sys_surface_destroy(g_wp_sid);   /* release previous */
            g_wp_sid=sid;
            mx_set_wallpaper(c, sid, img_w, img_h);
            scpy(msg,"wallpaper set", sizeof msg);
            return;
        }
        sys_surface_destroy(sid);
    }
    scpy(msg, "wallpaper saved (applies next boot)", sizeof msg);  /* surface failed; .mxrc persisted */
}

static int slen(const char*s){int n=0;while(s[n])n++;return n;}

/* ---- gallery ---- */
static int is_image_name(const char *nm){
    int n=slen(nm);
    if(n>=4 && nm[n-4]=='.'){
        char a=nm[n-3]|32, b=nm[n-2]|32, c=nm[n-1]|32;
        if((a=='b'&&b=='m'&&c=='p')||(a=='g'&&b=='i'&&c=='f')||
           (a=='p'&&b=='n'&&c=='g')||(a=='j'&&b=='p'&&c=='g')) return 1;
    }
    if(n>=5 && nm[n-5]=='.' && (nm[n-4]|32)=='j'&&(nm[n-3]|32)=='p'&&(nm[n-2]|32)=='e'&&(nm[n-1]|32)=='g') return 1;
    return 0;
}
static void gal_scan_dir(const char *dir){
    struct dirent de;
    for(unsigned i=0; gal_n<GAL_MAX; i++){
        if(sys_readdir(dir,i,&de)!=1) break;
        if(de.d_type==DT_DIR) continue;
        if(!is_image_name(de.d_name)) continue;
        int p=0; for(const char*q=dir;*q&&p<254;q++) gal_path[gal_n][p++]=*q;
        if(p&&gal_path[gal_n][p-1]!='/') gal_path[gal_n][p++]='/';
        for(int k=0; de.d_name[k]&&p<255; k++) gal_path[gal_n][p++]=de.d_name[k];
        gal_path[gal_n][p]=0;
        scpy(gal_name[gal_n], de.d_name, sizeof gal_name[0]);
        gal_n++;
    }
}
static void gal_scan(void){
    gal_n=0; gal_scroll=0;
    char pics[96]; mxrc_home("/Pictures", pics, sizeof pics);
    gal_scan_dir(pics);
    gal_scan_dir("/usr/share/pixmaps");
    gal_scan_dir("/usr/share/backgrounds");
    for(int i=0;i<GAL_MAX;i++) gal_ok[i]=0;
}
/* decode gal_path[i] into its thumbnail slot (aspect-fit); uses img_px as scratch */
static void gal_decode(int i){
    if(gal_ok[i]) return;
    gal_ok[i]=2;
    int fd=sys_open(gal_path[i],O_RDONLY); if(fd<0) return;
    long sz=sys_lseek(fd,0,SEEK_END); sys_lseek(fd,0,0);
    if(sz<=0||(unsigned long)sz>FILE_CAP){ sys_close(fd); return; }
    unsigned got=0; while(got<(unsigned)sz){ long r=sys_read(fd,fbuf+got,(unsigned)sz-got); if(r<=0)break; got+=(unsigned)r; }
    sys_close(fd);
    if(decode_image(got)!=0 || img_w<1) return;
    gfx_surface ts={ gal_thumb+(long)i*THUMB_W*THUMB_H, THUMB_W, THUMB_H };
    gfx_fill(&ts,0,0,THUMB_W,THUMB_H,COL_BG);
    int fw,fh;
    if((long)img_w*THUMB_H > (long)img_h*THUMB_W){ fw=THUMB_W; fh=(int)((long)img_h*THUMB_W/img_w); }
    else { fh=THUMB_H; fw=(int)((long)img_w*THUMB_H/img_h); }
    if(fw<1)fw=1; if(fh<1)fh=1;
    gfx_surface src={ img_px, img_w, img_h };
    gfx_blit_scaled(&ts,(THUMB_W-fw)/2,(THUMB_H-fh)/2,fw,fh,&src);
    gal_ok[i]=1;
}
/* draw the thumbnail grid; returns the clicked index or -1 */
static int gallery_render(gfx_surface *s, ui_ctx *u, int x,int y,int w,int h){
    if(gal_n==0){ gfx_str(s,x+8,y+8,"No images in ~/Pictures or /usr/share.",COL_TEXT); return -1; }
    int cw=THUMB_W+12, ch=THUMB_H+22, cols=w/cw; if(cols<1)cols=1;
    int rows=(gal_n+cols-1)/cols, visrows=h/ch; if(visrows<1)visrows=1;
    if(gal_scroll>rows-visrows) gal_scroll = rows-visrows>0?rows-visrows:0;
    if(gal_scroll<0) gal_scroll=0;
    int clicked=-1;
    for(int i=0;i<gal_n;i++){
        int r=i/cols-gal_scroll, col=i%cols;
        if(r<0||r>=visrows) continue;
        int cx=x+col*cw, cy=y+r*ch;
        int hot = u->mx>=cx&&u->mx<cx+cw&&u->my>=cy&&u->my<cy+ch;
        if(hot) gfx_outline(s,cx,cy,cw-2,ch-2,COL_TEXT);
        gal_decode(i);
        if(gal_ok[i]==1){ gfx_surface t={ gal_thumb+(long)i*THUMB_W*THUMB_H, THUMB_W, THUMB_H };
            gfx_blit(s,cx+6,cy+4,&t,0,0,THUMB_W,THUMB_H); }
        else { gfx_fill(s,cx+6,cy+4,THUMB_W,THUMB_H,COL_BAR); gfx_str(s,cx+6+8,cy+4+THUMB_H/2,"(bad)",COL_ERR); }
        char nm[20]; scpy(nm,gal_name[i],sizeof nm);
        gfx_str_clip(s,cx+6,cy+THUMB_H+8,nm,COL_TEXT,cx+cw-4);
        if(hot && u->mpressed) clicked=i;
    }
    return clicked;
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 600, 460, MX_F_RESIZABLE) != 0) return 1;
    img_px = (gfx_u32*)sys_mmap(0,(unsigned long)IMG_MAXW*IMG_MAXH*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    fbuf   = (unsigned char*)sys_mmap(0,FILE_CAP,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    gal_thumb = (gfx_u32*)sys_mmap(0,(unsigned long)GAL_MAX*THUMB_W*THUMB_H*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (img_px==(void*)MAP_FAILED || fbuf==(void*)MAP_FAILED || gal_thumb==(void*)MAP_FAILED) return 1;

    ui_ctx u; for(unsigned i=0;i<sizeof u/sizeof(int);i++)((int*)&u)[i]=0;
    browser brz; for(unsigned i=0;i<sizeof brz/sizeof(int);i++)((int*)&brz)[i]=0;
    int dlg=0;

    /* Optional path argument (skip -makx <pid>). */
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'){ i++; continue; }
        load_image(argv[i]); break;
    }

    int first=1, lmx=-1, lmy=-1;
    while(!c.closed){
        mx_pump(&c);
        int gk=-1, kk; while((kk=mx_key(&c))>=0) gk=kk;   /* last key (gallery scroll) */
        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||c.mpressed||c.mreleased||moved||c.resized||gk>=0)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);

        /* toolbar */
        gfx_fill(s,0,0,s->w,30,COL_BAR);
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        int open_c=ui_button(&u,s,6,5,64,20,"Open");
        int wp_c=ui_button(&u,s,74,5,112,20,"Set Wallpaper");
        int gal_c=ui_button(&u,s,190,5,70,20, g_gallery?"Viewer":"Gallery");
        gfx_str_clip(s,266,11,msg,COL_TEXT,s->w-8);
        if(open_c){ scpy(brz.cwd,"/apps",sizeof brz.cwd); brz.sel=brz.scroll=0; brz.loaded=0; br_load(&brz); dlg=1; g_gallery=0; }
        if(wp_c && img_w>0 && !g_gallery) set_wallpaper(&c);
        if(gal_c){ g_gallery=!g_gallery;
            if(g_gallery){ dlg=0; gal_scan(); }
            else if(g_cur_path[0]) load_image(g_cur_path);   /* thumbs clobbered img_px; restore the viewed image */
            else img_w=0;
        }

        if(g_gallery){
            if(gk==0x81) gal_scroll++;                 /* arrow down */
            else if(gk==0x80 && gal_scroll>0) gal_scroll--;  /* arrow up */
            int ci=gallery_render(s,&u,4,34,s->w-8,s->h-34-4);
            if(ci>=0){ load_image(gal_path[ci]); g_gallery=0; }
        } else if(dlg){
            char full[256];
            int r=br_dialog(&brz,&u,s,6,34,s->w-12,s->h-34-6,1,(char*)0,0,full,sizeof full);
            if(r==1){ load_image(full); dlg=0; }
            else if(r==2){ dlg=0; }
        } else if(img_w>0){
            /* aspect-fit the image into the area below the toolbar */
            int ax=4, ay=34, aw=s->w-8, ah=s->h-34-4;
            int fw, fh;
            if((long)img_w*ah > (long)img_h*aw){ fw=aw; fh=(int)((long)img_h*aw/img_w); }
            else { fh=ah; fw=(int)((long)img_w*ah/img_h); }
            if(fw<1)fw=1; if(fh<1)fh=1;
            int fx=ax+(aw-fw)/2, fy=ay+(ah-fh)/2;
            gfx_surface src={ img_px, img_w, img_h };
            gfx_blit_scaled(s, fx, fy, fw, fh, &src);
            /* size readout "WxH" in the corner */
            char dim[48]; int o=0;
            { int v=img_w; char t[8]; int ti=0; if(!v)t[ti++]='0'; while(v){t[ti++]=(char)('0'+v%10);v/=10;} while(ti)dim[o++]=t[--ti]; }
            dim[o++]='x'; { int v=img_h; char t[8]; int ti=0; if(!v)t[ti++]='0'; while(v){t[ti++]=(char)('0'+v%10);v/=10;} while(ti)dim[o++]=t[--ti]; }
            dim[o]=0;
            gfx_str(s, s->w-gfx_text_w(dim)-6, s->h-12, dim, COL_TEXT);
        } else {
            const char *h="No image. Click Open to choose a BMP, GIF, PNG or JPEG file.";
            gfx_str(s,(s->w-gfx_text_w(h))/2, s->h/2, h, (msg[0]&&msg[slen(msg)-1]!='.')?COL_ERR:COL_TEXT);
        }
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
