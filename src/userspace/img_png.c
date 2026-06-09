/*
 * img_png.c -- shared PNG decoder (see img_png.h).
 *
 * Two layers:
 *   1. inflate() -- an RFC1951 DEFLATE decompressor (puff-style: decode one
 *      Huffman symbol at a time, LSB-first bit reader).  Handles stored, fixed
 *      and dynamic-Huffman blocks.  Writes into a caller-sized output buffer.
 *   2. png_decode() -- parses the PNG signature + chunks (IHDR/PLTE/IDAT/IEND),
 *      concatenates the IDAT payload, inflates it, reverses the per-scanline
 *      filters (None/Sub/Up/Average/Paeth) and samples each pixel to XRGB8888.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "img_png.h"

#define RGB GFX_RGB

static unsigned be32(const unsigned char *p)
{ return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }

static void ecpy(char *d, const char *s, int cap)
{ if(!d||cap<=0) return; int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }

/* ---- RFC1951 inflate -------------------------------------------------- */

typedef struct {
    const unsigned char *in; unsigned inlen, inpos;
    unsigned bitbuf; int bitcnt;
    unsigned char *out; unsigned outcap, outpos;
} infl;

/* Read n bits LSB-first (n<=16).  Returns -1 if the input runs dry. */
static int bits(infl *s, int n)
{
    while (s->bitcnt < n) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf |= (unsigned)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << n) - 1u));
    s->bitbuf >>= n; s->bitcnt -= n;
    return v;
}

typedef struct { short counts[16]; short symbols[288]; } htree;

/* Build a canonical Huffman decode table from per-symbol code lengths. */
static void build(htree *t, const unsigned char *lengths, int n)
{
    int i, offs[16];
    for (i = 0; i < 16; i++) t->counts[i] = 0;
    for (i = 0; i < n; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    offs[0] = offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + t->counts[i];
    for (i = 0; i < n; i++) if (lengths[i]) t->symbols[offs[lengths[i]]++] = (short)i;
}

/* Decode one symbol (canonical codes are packed MSB-first). */
static int decode(infl *s, const htree *t)
{
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= 15; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        count = t->counts[len];
        if (code - first < count) return t->symbols[index + (code - first)];
        index += count;
        first += count; first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short L_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                 51,59,67,83,99,115,131,163,195,227,258};
static const short L_EXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,
                                 5,5,5,5,0};
static const short D_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,
                                 385,513,769,1025,1537,2049,3073,4097,6145,8193,
                                 12289,16385,24577};
static const short D_EXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,
                                 10,11,11,12,12,13,13};

/* Decode one compressed block body given its literal/length + distance trees. */
static int codes(infl *s, const htree *lc, const htree *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym == 256) return 0;            /* end of block */
        if (sym < 256) {
            if (s->outpos >= s->outcap) return -1;
            s->out[s->outpos++] = (unsigned char)sym;
            continue;
        }
        sym -= 257;
        if (sym >= 29) return -1;
        int e = bits(s, L_EXT[sym]); if (e < 0) return -1;
        int len = L_BASE[sym] + e;
        int dsym = decode(s, dc);
        if (dsym < 0 || dsym >= 30) return -1;
        int de = bits(s, D_EXT[dsym]); if (de < 0) return -1;
        unsigned dist = (unsigned)(D_BASE[dsym] + de);
        if (dist > s->outpos) return -1;     /* refers before output start */
        if (s->outpos + (unsigned)len > s->outcap) return -1;
        while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
    }
}

static int block_stored(infl *s)
{
    s->bitbuf = 0; s->bitcnt = 0;            /* align to byte boundary */
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | ((unsigned)s->in[s->inpos + 1] << 8);
    s->inpos += 4;                           /* skip LEN + NLEN */
    if (s->inpos + len > s->inlen) return -1;
    if (s->outpos + len > s->outcap) return -1;
    while (len--) s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}

static int block_fixed(infl *s)
{
    unsigned char ll[288], dl[30];
    int i;
    for (i = 0;   i < 144; i++) ll[i] = 8;
    for (;        i < 256; i++) ll[i] = 9;
    for (;        i < 280; i++) ll[i] = 7;
    for (;        i < 288; i++) ll[i] = 8;
    for (i = 0;   i < 30;  i++) dl[i] = 5;
    htree lc, dc; build(&lc, ll, 288); build(&dc, dl, 30);
    return codes(s, &lc, &dc);
}

static int block_dynamic(infl *s)
{
    static const unsigned char ord[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = bits(s, 5); int hdist = bits(s, 5); int hclen = bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    hlit += 257; hdist += 1; hclen += 4;

    unsigned char cl[19]; int i;
    for (i = 0; i < 19; i++) cl[i] = 0;
    for (i = 0; i < hclen; i++) { int v = bits(s, 3); if (v < 0) return -1; cl[ord[i]] = (unsigned char)v; }
    htree clc; build(&clc, cl, 19);

    unsigned char lengths[288 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode(s, &clc);
        if (sym < 0) return -1;
        if (sym < 16) { lengths[n++] = (unsigned char)sym; }
        else if (sym == 16) {
            if (n == 0) return -1;
            int r = bits(s, 2); if (r < 0) return -1; r += 3;
            unsigned char prev = lengths[n - 1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int r = bits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else { /* 18 */
            int r = bits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    htree lc, dc;
    build(&lc, lengths, hlit);
    build(&dc, lengths + hlit, hdist);
    return codes(s, &lc, &dc);
}

/* Inflate a raw DEFLATE stream from in[0..inlen) into out[0..outcap).
 * Returns the number of bytes produced, or -1 on error. */
static long inflate(const unsigned char *in, unsigned inlen,
                    unsigned char *out, unsigned outcap)
{
    infl s; s.in = in; s.inlen = inlen; s.inpos = 0;
    s.bitbuf = 0; s.bitcnt = 0; s.out = out; s.outcap = outcap; s.outpos = 0;
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        int rc;
        if (type == 0)      rc = block_stored(&s);
        else if (type == 1) rc = block_fixed(&s);
        else if (type == 2) rc = block_dynamic(&s);
        else                return -1;       /* reserved */
        if (rc != 0) return -1;
    } while (!last);
    return (long)s.outpos;
}

/* ---- PNG ------------------------------------------------------------- */

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* channels per colour type: 0 gray,2 rgb,3 indexed,4 gray+a,6 rgba */
static int channels_of(int ct)
{
    switch (ct) { case 0: return 1; case 2: return 3; case 3: return 1;
                  case 4: return 2; case 6: return 4; default: return 0; }
}

int png_decode(const unsigned char *file, unsigned n, gfx_u32 *out,
               int out_max_w, int out_max_h, int *w, int *h,
               char *err, int errcap)
{
    static const unsigned char sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    unsigned char *raw = (unsigned char *)MAP_FAILED;
    unsigned char *zin = (unsigned char *)MAP_FAILED;
    unsigned long raw_cap = 0, zin_cap = 0;
    int ret = -1;

    if (n < 8) { ecpy(err, "truncated PNG", errcap); return -1; }
    for (int i = 0; i < 8; i++)
        if (file[i] != sig[i]) { ecpy(err, "not a PNG file", errcap); return -1; }

    int iw = 0, ih = 0, depth = 0, ct = -1, interlace = 0;
    unsigned char pal[256][3]; int have_ihdr = 0;

    /* First pass: read IHDR + PLTE, and measure the total IDAT length. */
    unsigned p = 8, idat_total = 0;
    while (p + 8 <= n) {
        unsigned clen = be32(file + p);
        const unsigned char *tag = file + p + 4;
        unsigned cdata = p + 8;
        if (cdata + clen + 4 > n) { ecpy(err, "truncated PNG", errcap); return -1; }
        if (tag[0]=='I'&&tag[1]=='H'&&tag[2]=='D'&&tag[3]=='R') {
            if (clen < 13) { ecpy(err, "bad PNG header", errcap); return -1; }
            iw = (int)be32(file + cdata); ih = (int)be32(file + cdata + 4);
            depth = file[cdata + 8]; ct = file[cdata + 9];
            interlace = file[cdata + 12];
            have_ihdr = 1;
        } else if (tag[0]=='P'&&tag[1]=='L'&&tag[2]=='T'&&tag[3]=='E') {
            unsigned ne = clen / 3; if (ne > 256) ne = 256;
            for (unsigned i = 0; i < ne; i++) {
                pal[i][0] = file[cdata + i*3];
                pal[i][1] = file[cdata + i*3 + 1];
                pal[i][2] = file[cdata + i*3 + 2];
            }
        } else if (tag[0]=='I'&&tag[1]=='D'&&tag[2]=='A'&&tag[3]=='T') {
            idat_total += clen;
        } else if (tag[0]=='I'&&tag[1]=='E'&&tag[2]=='N'&&tag[3]=='D') {
            break;
        }
        p = cdata + clen + 4;            /* skip data + CRC */
    }

    if (!have_ihdr) { ecpy(err, "bad PNG header", errcap); return -1; }
    int nch = channels_of(ct);
    if (nch == 0) { ecpy(err, "unsupported PNG colour type", errcap); return -1; }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
        ecpy(err, "unsupported PNG bit depth", errcap); return -1;
    }
    if (interlace) { ecpy(err, "interlaced PNG unsupported", errcap); return -1; }
    if (iw < 1 || ih < 1 || iw > out_max_w || ih > out_max_h) {
        ecpy(err, "image too large", errcap); return -1;
    }
    if (idat_total == 0) { ecpy(err, "no image data", errcap); return -1; }

    /* Gather the (possibly multi-chunk) IDAT payload into one buffer. */
    zin_cap = idat_total;
    zin = (unsigned char *)sys_mmap(0, zin_cap, PROT_READ|PROT_WRITE,
                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (zin == (unsigned char *)MAP_FAILED) { ecpy(err, "out of memory", errcap); return -1; }
    {
        unsigned zp = 0; p = 8;
        while (p + 8 <= n) {
            unsigned clen = be32(file + p);
            const unsigned char *tag = file + p + 4;
            unsigned cdata = p + 8;
            if (tag[0]=='I'&&tag[1]=='D'&&tag[2]=='A'&&tag[3]=='T') {
                for (unsigned i = 0; i < clen && zp < zin_cap; i++) zin[zp++] = file[cdata + i];
            } else if (tag[0]=='I'&&tag[1]=='E'&&tag[2]=='N'&&tag[3]=='D') break;
            p = cdata + clen + 4;
        }
    }

    /* zlib wrapper: 2-byte header, then raw DEFLATE (adler32 trailer ignored). */
    if (zin_cap < 2) { ecpy(err, "bad PNG stream", errcap); goto done; }

    int filterbpp = (nch * depth + 7) / 8; if (filterbpp < 1) filterbpp = 1;
    unsigned long stride = ((unsigned long)iw * nch * depth + 7) / 8;
    raw_cap = (stride + 1) * (unsigned long)ih;
    raw = (unsigned char *)sys_mmap(0, raw_cap, PROT_READ|PROT_WRITE,
                                    MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (raw == (unsigned char *)MAP_FAILED) { ecpy(err, "out of memory", errcap); goto done; }

    long got = inflate(zin + 2, zin_cap - 2, raw, raw_cap);
    if (got < 0 || (unsigned long)got < raw_cap) { ecpy(err, "corrupt PNG stream", errcap); goto done; }

    /* Reverse the per-scanline filters in place, then sample to XRGB8888. */
    int maxval = (1 << (depth > 8 ? 8 : depth)) - 1;   /* for gray scaling */
    for (int y = 0; y < ih; y++) {
        unsigned char *row = raw + (unsigned long)y * (stride + 1);
        int ftype = row[0];
        unsigned char *cur = row + 1;
        unsigned char *prev = (y > 0) ? raw + (unsigned long)(y - 1) * (stride + 1) + 1 : 0;
        for (unsigned long i = 0; i < stride; i++) {
            int a = (i >= (unsigned long)filterbpp) ? cur[i - filterbpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (unsigned long)filterbpp) ? prev[i - filterbpp] : 0;
            int x = cur[i];
            switch (ftype) {
                case 0: break;
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) >> 1; break;
                case 4: x += paeth(a, b, c); break;
                default: ecpy(err, "bad PNG filter", errcap); goto done;
            }
            cur[i] = (unsigned char)x;
        }
        /* Sample each pixel out of the unfiltered scanline. */
        gfx_u32 *dst = out + (unsigned long)y * iw;
        for (int xp = 0; xp < iw; xp++) {
            int s[4] = {0,0,0,0};
            for (int ch = 0; ch < nch; ch++) {
                int raw_s;
                if (depth == 8)       raw_s = cur[xp * nch + ch];
                else if (depth == 16) raw_s = cur[(xp * nch + ch) * 2];   /* high byte */
                else {
                    int bitpos = (xp * nch + ch) * depth;
                    int byte = cur[bitpos >> 3];
                    int shift = 8 - depth - (bitpos & 7);
                    raw_s = (byte >> shift) & ((1 << depth) - 1);
                }
                s[ch] = raw_s;
            }
            int r, g, bl, a = 255;
            if (ct == 3) {                 /* indexed: s[0] is a palette index */
                int idx = s[0] & 0xff;
                r = pal[idx][0]; g = pal[idx][1]; bl = pal[idx][2];
            } else if (ct == 0 || ct == 4) { /* grayscale (+alpha) */
                int v = (depth < 8) ? s[0] * 255 / maxval : s[0];
                r = g = bl = v;
                if (ct == 4) a = (depth < 8) ? s[1] * 255 / maxval : s[1];
            } else {                       /* truecolour (+alpha) */
                r = s[0]; g = s[1]; bl = s[2];
                if (ct == 6) a = (depth < 8) ? s[3] * 255 / maxval : s[3];
            }
            /* No alpha channel in XRGB8888 -- composite over white so transparent
             * regions read as the page/background instead of a black box. */
            if (a < 255) {
                r  = (r  * a + 255 * (255 - a) + 127) / 255;
                g  = (g  * a + 255 * (255 - a) + 127) / 255;
                bl = (bl * a + 255 * (255 - a) + 127) / 255;
            }
            dst[xp] = RGB(r, g, bl);
        }
    }

    *w = iw; *h = ih;
    ret = 0;

done:
    if (raw != (unsigned char *)MAP_FAILED) sys_munmap(raw, raw_cap);
    if (zin != (unsigned char *)MAP_FAILED) sys_munmap(zin, zin_cap);
    return ret;
}

/* Bound for png_load's one-shot decode (covers icons + small wallpapers). */
#define PNG_LOAD_MAXW 1024
#define PNG_LOAD_MAXH 1024

int png_load(const char *path, gfx_surface *out)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long sz = sys_lseek(fd, 0, SEEK_END);
    sys_lseek(fd, 0, 0);
    if (sz <= 24 || sz > 8L * 1024 * 1024) { sys_close(fd); return -1; }

    unsigned long cap = (unsigned long)sz;
    unsigned char *fbuf = (unsigned char *)sys_mmap(0, cap, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (fbuf == (unsigned char *)MAP_FAILED) { sys_close(fd); return -1; }
    unsigned got = 0;
    while (got < (unsigned)sz) {
        long r = sys_read(fd, fbuf + got, (unsigned)sz - got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    sys_close(fd);

    /* Peek IHDR dimensions (big-endian, bytes 16..23) to size the buffer. */
    if (got < 24) { sys_munmap(fbuf, cap); return -1; }
    int iw = (int)be32(fbuf + 16), ih = (int)be32(fbuf + 20);
    if (iw < 1 || ih < 1 || iw > PNG_LOAD_MAXW || ih > PNG_LOAD_MAXH) {
        sys_munmap(fbuf, cap); return -1;
    }
    unsigned long pxbytes = (unsigned long)iw * ih * 4u;
    gfx_u32 *px = (gfx_u32 *)sys_mmap(0, pxbytes, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (px == (gfx_u32 *)MAP_FAILED) { sys_munmap(fbuf, cap); return -1; }

    int dw, dh;
    if (png_decode(fbuf, got, px, iw, ih, &dw, &dh, 0, 0) != 0) {
        sys_munmap(px, pxbytes); sys_munmap(fbuf, cap); return -1;
    }
    sys_munmap(fbuf, cap);
    out->px = px; out->w = dw; out->h = dh;
    return 0;
}
