/*
 * mximg.elf -- an image viewer, as a makx client.  Opens an image (Open dialog
 * or a path argument), decodes it, and scales it to fit the window (aspect-
 * preserving, nearest-neighbour).  This first cut decodes BMP (24/32-bpp
 * uncompressed); GIF/PNG/JPEG land next.  Reuses the shared gui_browser file
 * dialog (like mxedit) so it's usable straight from its desktop icon.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "gui_ui.h"
#include "gui_browser.h"
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
static char msg[96] = "Open an image (BMP).";

static void scpy(char *d,const char *s,int max){int i=0;while(s[i]&&i<max-1){d[i]=s[i];i++;}d[i]=0;}
static unsigned rd32(const unsigned char *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }
static int      rd16(const unsigned char *p){ return p[0]|(p[1]<<8); }

/* Decode an uncompressed 24/32-bpp BMP from fbuf[0..n) into img_px. 0 ok. */
static int decode_bmp(unsigned n)
{
    if (n < 54 || fbuf[0] != 'B' || fbuf[1] != 'M') { scpy(msg,"not a BMP file",sizeof msg); return -1; }
    unsigned off = rd32(fbuf+10);
    int w = (int)rd32(fbuf+18);
    int hh = (int)rd32(fbuf+22);
    int bpp = rd16(fbuf+28);
    unsigned comp = rd32(fbuf+30);
    int topdown = 0;
    if (hh < 0) { hh = -hh; topdown = 1; }
    if (comp != 0 || (bpp != 24 && bpp != 32)) { scpy(msg,"unsupported BMP (need 24/32-bpp uncompressed)",sizeof msg); return -1; }
    if (w < 1 || hh < 1 || w > IMG_MAXW || hh > IMG_MAXH) { scpy(msg,"image too large",sizeof msg); return -1; }
    int bypp = bpp/8;
    unsigned stride = ((unsigned)(w*bypp) + 3u) & ~3u;
    if (off + stride*(unsigned)hh > n) { scpy(msg,"truncated BMP",sizeof msg); return -1; }
    for (int y = 0; y < hh; y++) {
        int srcrow = topdown ? y : (hh-1-y);
        const unsigned char *row = fbuf + off + (unsigned)srcrow*stride;
        gfx_u32 *dst = img_px + (unsigned)y*w;
        for (int x = 0; x < w; x++) {
            const unsigned char *px = row + x*bypp;
            dst[x] = RGB(px[2], px[1], px[0]);   /* BMP is BGR(A) */
        }
    }
    img_w = w; img_h = hh;
    return 0;
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
    if (decode_bmp(got) == 0) {
        /* path basename into msg */
        const char *b = path; for (const char *p=path; *p; p++) if (*p=='/') b=p+1;
        scpy(msg, b, sizeof msg);
    }
}

static int slen(const char*s){int n=0;while(s[n])n++;return n;}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 600, 460, MX_F_RESIZABLE) != 0) return 1;
    img_px = (gfx_u32*)sys_mmap(0,(unsigned long)IMG_MAXW*IMG_MAXH*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    fbuf   = (unsigned char*)sys_mmap(0,FILE_CAP,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (img_px==(void*)MAP_FAILED || fbuf==(void*)MAP_FAILED) return 1;

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
        while(mx_key(&c)>=0){ /* keys go to the dialog via ui */ }
        int moved=(c.mx!=lmx||c.my!=lmy); lmx=c.mx; lmy=c.my;
        if(!(first||c.mpressed||c.mreleased||moved||c.resized)){ sys_yield(); continue; }
        first=0;

        gfx_surface *s=&c.surf;
        gfx_fill(s,0,0,s->w,s->h,COL_BG);

        /* toolbar */
        gfx_fill(s,0,0,s->w,30,COL_BAR);
        ui_begin(&u,c.mx,c.my,c.mdown,c.mpressed,c.mreleased,-1);
        int open_c=ui_button(&u,s,6,5,64,20,"Open");
        gfx_str_clip(s,80,11,msg,COL_TEXT,s->w-8);
        if(open_c){ scpy(brz.cwd,"/apps",sizeof brz.cwd); brz.sel=brz.scroll=0; brz.loaded=0; br_load(&brz); dlg=1; }

        if(dlg){
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
            const char *h="No image. Click Open to choose a BMP file.";
            gfx_str(s,(s->w-gfx_text_w(h))/2, s->h/2, h, (msg[0]&&msg[slen(msg)-1]!='.')?COL_ERR:COL_TEXT);
        }
        mx_present(&c);
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
