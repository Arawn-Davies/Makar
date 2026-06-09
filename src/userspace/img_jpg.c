/*
 * img_jpg.c -- shared baseline-JPEG decoder (see img_jpg.h).
 *
 * Pipeline: marker walk -> per-component Huffman entropy decode (DC diff + AC
 * run/size) -> dequantise -> fixed-point separable 8x8 IDCT -> chroma upsample
 * -> YCbCr->RGB.  Integer-only; scratch component planes come from sys_mmap.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "img_jpg.h"

#define RGB GFX_RGB

static void ecpy(char *d, const char *s, int cap)
{ if(!d||cap<=0) return; int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }

/* zigzag[k] = natural 8x8 index of the k-th coefficient in JPEG scan order */
static const unsigned char ZZ[64] = {
   0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
  12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
  58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

/* A[u][x] = C(u)*cos((2x+1)u*pi/16) << 12 (C(0)=1/sqrt2) -- fixed-point IDCT */
static const int IDCT_A[8][8] = {
  {  2896,  2896,  2896,  2896,  2896,  2896,  2896,  2896},
  {  4017,  3406,  2276,   799,  -799, -2276, -3406, -4017},
  {  3784,  1567, -1567, -3784, -3784, -1567,  1567,  3784},
  {  3406,  -799, -4017, -2276,  2276,  4017,   799, -3406},
  {  2896, -2896, -2896,  2896,  2896, -2896, -2896,  2896},
  {  2276, -4017,   799,  3406, -3406,  -799,  4017, -2276},
  {  1567, -3784,  3784, -1567, -1567,  3784, -3784,  1567},
  {   799, -2276,  3406, -4017,  4017, -3406,  2276,  -799},
};

/* 2D inverse DCT of `blk` (natural order, dequantised) -> `out` (8 bytes/row,
 * level-shifted to 0..255).  Two 1D passes with a >>11 then >>15 descale. */
static void idct8x8(const int *blk, unsigned char *out, int stride)
{
    int tmp[64];
    for (int v = 0; v < 8; v++)                /* pass 1: rows (horizontal) */
        for (int x = 0; x < 8; x++) {
            int s = 0;
            for (int u = 0; u < 8; u++) s += IDCT_A[u][x] * blk[v*8 + u];
            tmp[v*8 + x] = s >> 11;
        }
    for (int y = 0; y < 8; y++)                /* pass 2: columns (vertical) */
        for (int x = 0; x < 8; x++) {
            int s = 0;
            for (int v = 0; v < 8; v++) s += IDCT_A[v][y] * tmp[v*8 + x];
            int p = (s >> 15) + 128;
            out[y*stride + x] = (unsigned char)(p < 0 ? 0 : p > 255 ? 255 : p);
        }
}

/* ---- bit reader over the entropy-coded segment (MSB-first, FF de-stuffing) -- */
typedef struct { const unsigned char *d; unsigned n, pos; unsigned buf; int cnt; int marker; } bitrd;

static int jbyte(bitrd *b)
{
    if (b->marker) return 0;                   /* stalled at a marker -> feed 0s */
    if (b->pos >= b->n) return 0;
    unsigned char c = b->d[b->pos++];
    if (c == 0xFF) {
        while (b->pos < b->n && b->d[b->pos] == 0xFF) b->pos++;
        unsigned char m = (b->pos < b->n) ? b->d[b->pos] : 0;
        if (m == 0x00) { b->pos++; return 0xFF; }   /* stuffed FF 00 -> data FF */
        b->marker = m; return 0;                    /* real marker: stop here */
    }
    return c;
}
static int jbit(bitrd *b)
{ if (b->cnt == 0) { b->buf = (unsigned)jbyte(b); b->cnt = 8; } b->cnt--; return (int)((b->buf >> b->cnt) & 1); }
static int jbits(bitrd *b, int k) { int v = 0; while (k-- > 0) v = (v << 1) | jbit(b); return v; }
/* receive s bits then sign-extend per JPEG EXTEND() */
static int jrecv_ext(bitrd *b, int s)
{ if (s == 0) return 0; int v = jbits(b, s); if (v < (1 << (s - 1))) v -= (1 << s) - 1; return v; }
static void jbit_reset(bitrd *b) { b->cnt = 0; b->buf = 0; b->marker = 0; }

/* ---- Huffman table (JPEG Annex F mincode/maxcode/valptr) ---- */
typedef struct { unsigned char vals[256]; int mincode[17], maxcode[17], valptr[17]; } huff;
static void huff_build(huff *h, const unsigned char *counts)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        if (counts[l-1]) { code += counts[l-1]; k += counts[l-1]; h->maxcode[l] = code - 1; }
        else h->maxcode[l] = -1;
        code <<= 1;
    }
}
static int huff_decode(bitrd *b, const huff *h)
{
    int code = jbit(b), l = 1;
    while (h->maxcode[l] < 0 || code > h->maxcode[l]) {
        if (l >= 16) return 0;
        code = (code << 1) | jbit(b); l++;
    }
    return h->vals[h->valptr[l] + (code - h->mincode[l])];
}

static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

int jpg_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap)
{
    if (n < 2 || file[0] != 0xFF || file[1] != 0xD8) { ecpy(err, "not a JPEG", errcap); return -1; }

    unsigned short qt[4][64]; int have_qt[4] = {0,0,0,0};
    huff hdc[4], hac[4]; int have_hdc[4]={0}, have_hac[4]={0};
    int iw=0, ih=0, ncomp=0, restart=0;
    struct { int id, hs, vs, qt, dct, act; } comp[3];
    for (int i=0;i<3;i++){ comp[i].id=comp[i].hs=comp[i].vs=comp[i].qt=comp[i].dct=comp[i].act=0; }

    unsigned p = 2;
    /* ---- parse headers up to and including SOS ---- */
    for (;;) {
        if (p + 2 > n) { ecpy(err, "truncated JPEG", errcap); return -1; }
        if (file[p] != 0xFF) { p++; continue; }
        unsigned char m = file[p+1]; p += 2;
        if (m == 0xD9) { ecpy(err, "no image (EOI)", errcap); return -1; }   /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;                 /* standalone */
        if (p + 2 > n) { ecpy(err, "truncated JPEG", errcap); return -1; }
        unsigned seglen = rd16(file + p);
        if (seglen < 2 || p + seglen > n) { ecpy(err, "bad JPEG segment", errcap); return -1; }
        const unsigned char *s = file + p + 2; unsigned slen = seglen - 2;

        if (m == 0xC0 || m == 0xC1) {                       /* SOF0/1 baseline */
            if (slen < 6) { ecpy(err,"bad SOF",errcap); return -1; }
            if (s[0] != 8) { ecpy(err,"only 8-bit JPEG",errcap); return -1; }
            ih = (int)rd16(s+1); iw = (int)rd16(s+3); ncomp = s[5];
            if (ncomp != 1 && ncomp != 3) { ecpy(err,"unsupported JPEG components",errcap); return -1; }
            if (iw<1||ih<1||iw>out_max_w||ih>out_max_h) { ecpy(err,"image too large",errcap); return -1; }
            for (int c=0;c<ncomp;c++){ const unsigned char *cc=s+6+c*3;
                comp[c].id=cc[0]; comp[c].hs=cc[1]>>4; comp[c].vs=cc[1]&15; comp[c].qt=cc[2];
                if(comp[c].hs<1||comp[c].vs<1){ ecpy(err,"bad sampling",errcap); return -1; } }
        } else if (m == 0xC2) { ecpy(err,"progressive JPEG unsupported",errcap); return -1; }
        else if (m == 0xC3 || (m>=0xC5&&m<=0xCB&&m!=0xC8) || (m>=0xCD&&m<=0xCF)) { ecpy(err,"unsupported JPEG mode",errcap); return -1; }
        else if (m == 0xC8 || m == 0xCC) { ecpy(err,"unsupported JPEG",errcap); return -1; }
        else if (m == 0xDB) {                               /* DQT */
            unsigned q = 0;
            while (q < slen) { int pq = s[q]>>4, tq = s[q]&15; q++;
                if (tq>3) { ecpy(err,"bad DQT",errcap); return -1; }
                for (int k=0;k<64;k++){ qt[tq][k] = pq ? rd16(s+q+k*2) : s[q+k]; }
                q += pq?128:64; have_qt[tq]=1; }
        } else if (m == 0xC4) {                             /* DHT */
            unsigned q = 0;
            while (q + 17 <= slen) { int tc=s[q]>>4, th=s[q]&15; q++;
                if (th>3) { ecpy(err,"bad DHT",errcap); return -1; }
                const unsigned char *counts=s+q; int tot=0; for(int l=0;l<16;l++) tot+=counts[l];
                q += 16; if (q+tot>slen||tot>256) { ecpy(err,"bad DHT",errcap); return -1; }
                huff *ht = tc ? &hac[th] : &hdc[th];
                huff_build(ht, counts);
                for (int k=0;k<tot;k++) ht->vals[k]=s[q+k];
                q += tot; if (tc) have_hac[th]=1; else have_hdc[th]=1; }
        } else if (m == 0xDD) {                             /* DRI */
            if (slen>=2) restart = (int)rd16(s);
        } else if (m == 0xDA) {                             /* SOS -> entropy data follows */
            int ns = s[0];
            if (ns != ncomp) { ecpy(err,"SOS/SOF component mismatch",errcap); return -1; }
            for (int i=0;i<ns;i++){ int id=s[1+i*2], td=s[2+i*2]>>4, ta=s[2+i*2]&15;
                for (int c=0;c<ncomp;c++) if (comp[c].id==id){ comp[c].dct=td; comp[c].act=ta; } }
            p += seglen;                                    /* entropy data starts here */
            break;
        }
        p += seglen;
    }
    if (!iw || !ih) { ecpy(err,"no SOF",errcap); return -1; }

    /* ---- geometry ---- */
    int hmax=1, vmax=1;
    for (int c=0;c<ncomp;c++){ if(comp[c].hs>hmax)hmax=comp[c].hs; if(comp[c].vs>vmax)vmax=comp[c].vs; }
    int mcuw=8*hmax, mcuh=8*vmax;
    int mcux=(iw+mcuw-1)/mcuw, mcuy=(ih+mcuh-1)/mcuh;

    /* per-component sampled plane (mcux*hs*8) x (mcuy*vs*8) */
    unsigned char *plane[3]={0,0,0}; unsigned long psz[3]={0,0,0}; int pw[3],ph[3];
    int ret=-1;
    for (int c=0;c<ncomp;c++){
        pw[c]=mcux*comp[c].hs*8; ph[c]=mcuy*comp[c].vs*8;
        psz[c]=(unsigned long)pw[c]*ph[c];
        plane[c]=(unsigned char*)sys_mmap(0,psz[c],PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (plane[c]==(unsigned char*)MAP_FAILED){ plane[c]=0; ecpy(err,"out of memory",errcap); goto done; }
    }

    /* ---- entropy decode all MCUs ---- */
    {
        bitrd b; b.d=file; b.n=n; b.pos=p; b.buf=0; b.cnt=0; b.marker=0;
        int pred[3]={0,0,0};
        int rcnt=restart;
        for (int my=0;my<mcuy;my++) for (int mx=0;mx<mcux;mx++){
            for (int c=0;c<ncomp;c++){
                if (!have_hdc[comp[c].dct] || !have_hac[comp[c].act] || !have_qt[comp[c].qt]){ ecpy(err,"missing JPEG table",errcap); goto done; }
                for (int by=0;by<comp[c].vs;by++) for (int bx=0;bx<comp[c].hs;bx++){
                    int blk[64]; for(int i=0;i<64;i++) blk[i]=0;
                    int t = huff_decode(&b, &hdc[comp[c].dct]);
                    pred[c] += jrecv_ext(&b, t);
                    blk[0] = pred[c] * (int)qt[comp[c].qt][0];
                    int k=1;
                    while (k<64){
                        int rs=huff_decode(&b,&hac[comp[c].act]);
                        int r=rs>>4, sz=rs&15;
                        if (sz==0){ if(r==15){ k+=16; continue; } break; } /* ZRL / EOB */
                        k+=r; if(k>63) break;
                        blk[ZZ[k]] = jrecv_ext(&b,sz) * (int)qt[comp[c].qt][k];
                        k++;
                    }
                    int ox=(mx*comp[c].hs+bx)*8, oy=(my*comp[c].vs+by)*8;
                    idct8x8(blk, plane[c] + (unsigned long)oy*pw[c] + ox, pw[c]);
                }
            }
            /* restart interval: realign + reset DC predictors at RSTn */
            if (restart && --rcnt==0 && !(my==mcuy-1 && mx==mcux-1)){
                jbit_reset(&b);
                /* skip the RST marker in the stream */
                while (b.pos+1<n && !(file[b.pos]==0xFF && file[b.pos+1]>=0xD0 && file[b.pos+1]<=0xD7)) b.pos++;
                if (b.pos+1<n) b.pos+=2;
                pred[0]=pred[1]=pred[2]=0; rcnt=restart;
            }
        }
    }

    /* ---- upsample + YCbCr->RGB into out (top-down, pitch=iw) ---- */
    for (int y=0;y<ih;y++){
        gfx_u32 *drow = out + (unsigned long)y*iw;
        for (int x=0;x<iw;x++){
            int Y = plane[0][ (unsigned long)(y*comp[0].vs/vmax)*pw[0] + (x*comp[0].hs/hmax) ];
            int r,g,bl;
            if (ncomp==1){ r=g=bl=Y; }
            else {
                int cb = plane[1][ (unsigned long)(y*comp[1].vs/vmax)*pw[1] + (x*comp[1].hs/hmax) ] - 128;
                int cr = plane[2][ (unsigned long)(y*comp[2].vs/vmax)*pw[2] + (x*comp[2].hs/hmax) ] - 128;
                r  = Y + ((91881*cr) >> 16);
                g  = Y - ((22554*cb + 46802*cr) >> 16);
                bl = Y + ((116130*cb) >> 16);
                if(r<0)r=0; if(r>255)r=255; if(g<0)g=0; if(g>255)g=255; if(bl<0)bl=0; if(bl>255)bl=255;
            }
            drow[x] = RGB(r,g,bl);
        }
    }
    *w=iw; *h=ih; ret=0;

done:
    for (int c=0;c<3;c++) if (plane[c]) sys_munmap(plane[c], psz[c]);
    return ret;
}
