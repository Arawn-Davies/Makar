#ifndef _MAKAR_INFLATE_H
#define _MAKAR_INFLATE_H

/*
 * inflate.h -- a compact RFC1951 DEFLATE decompressor (puff-style), header-only
 * with internal linkage so each translation unit gets its own copy (no link
 * wiring).  Shared by img_png (zlib streams) and tar (gzip streams).
 *
 *   long inflate(const unsigned char *in, unsigned inlen,
 *                unsigned char *out, unsigned outcap);
 *      -> bytes produced into out, or -1 on error.
 */

typedef struct {
    const unsigned char *in; unsigned inlen, inpos;
    unsigned bitbuf; int bitcnt;
    unsigned char *out; unsigned outcap, outpos;
} infl;

/* Read n bits LSB-first (n<=16).  Returns -1 if the input runs dry. */
static int infl_bits(infl *s, int n)
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

typedef struct { short counts[16]; short symbols[288]; } infl_htree;

static void infl_build(infl_htree *t, const unsigned char *lengths, int n)
{
    int i, offs[16];
    for (i = 0; i < 16; i++) t->counts[i] = 0;
    for (i = 0; i < n; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    offs[0] = offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + t->counts[i];
    for (i = 0; i < n; i++) if (lengths[i]) t->symbols[offs[lengths[i]]++] = (short)i;
}

static int infl_decode(infl *s, const infl_htree *t)
{
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= 15; len++) {
        int b = infl_bits(s, 1);
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

static const short INFL_L_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                      51,59,67,83,99,115,131,163,195,227,258};
static const short INFL_L_EXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,
                                      5,5,5,5,0};
static const short INFL_D_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,
                                      385,513,769,1025,1537,2049,3073,4097,6145,8193,
                                      12289,16385,24577};
static const short INFL_D_EXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,
                                      10,11,11,12,12,13,13};

static int infl_codes(infl *s, const infl_htree *lc, const infl_htree *dc)
{
    for (;;) {
        int sym = infl_decode(s, lc);
        if (sym < 0) return -1;
        if (sym == 256) return 0;
        if (sym < 256) {
            if (s->outpos >= s->outcap) return -1;
            s->out[s->outpos++] = (unsigned char)sym;
            continue;
        }
        sym -= 257;
        if (sym >= 29) return -1;
        int e = infl_bits(s, INFL_L_EXT[sym]); if (e < 0) return -1;
        int len = INFL_L_BASE[sym] + e;
        int dsym = infl_decode(s, dc);
        if (dsym < 0 || dsym >= 30) return -1;
        int de = infl_bits(s, INFL_D_EXT[dsym]); if (de < 0) return -1;
        unsigned dist = (unsigned)(INFL_D_BASE[dsym] + de);
        if (dist > s->outpos) return -1;
        if (s->outpos + (unsigned)len > s->outcap) return -1;
        while (len--) { s->out[s->outpos] = s->out[s->outpos - dist]; s->outpos++; }
    }
}

static int infl_block_stored(infl *s)
{
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | ((unsigned)s->in[s->inpos + 1] << 8);
    s->inpos += 4;
    if (s->inpos + len > s->inlen) return -1;
    if (s->outpos + len > s->outcap) return -1;
    while (len--) s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}

static int infl_block_fixed(infl *s)
{
    unsigned char ll[288], dl[30];
    int i;
    for (i = 0;   i < 144; i++) ll[i] = 8;
    for (;        i < 256; i++) ll[i] = 9;
    for (;        i < 280; i++) ll[i] = 7;
    for (;        i < 288; i++) ll[i] = 8;
    for (i = 0;   i < 30;  i++) dl[i] = 5;
    infl_htree lc, dc; infl_build(&lc, ll, 288); infl_build(&dc, dl, 30);
    return infl_codes(s, &lc, &dc);
}

static int infl_block_dynamic(infl *s)
{
    static const unsigned char ord[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit = infl_bits(s, 5); int hdist = infl_bits(s, 5); int hclen = infl_bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
    hlit += 257; hdist += 1; hclen += 4;

    unsigned char cl[19]; int i;
    for (i = 0; i < 19; i++) cl[i] = 0;
    for (i = 0; i < hclen; i++) { int v = infl_bits(s, 3); if (v < 0) return -1; cl[ord[i]] = (unsigned char)v; }
    infl_htree clc; infl_build(&clc, cl, 19);

    unsigned char lengths[288 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = infl_decode(s, &clc);
        if (sym < 0) return -1;
        if (sym < 16) { lengths[n++] = (unsigned char)sym; }
        else if (sym == 16) {
            if (n == 0) return -1;
            int r = infl_bits(s, 2); if (r < 0) return -1; r += 3;
            unsigned char prev = lengths[n - 1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int r = infl_bits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else {
            int r = infl_bits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    infl_htree lc, dc;
    infl_build(&lc, lengths, hlit);
    infl_build(&dc, lengths + hlit, hdist);
    return infl_codes(s, &lc, &dc);
}

/* Inflate a raw DEFLATE stream into out[0..outcap).  Returns bytes produced or -1. */
static long inflate(const unsigned char *in, unsigned inlen,
                    unsigned char *out, unsigned outcap)
{
    infl s; s.in = in; s.inlen = inlen; s.inpos = 0;
    s.bitbuf = 0; s.bitcnt = 0; s.out = out; s.outcap = outcap; s.outpos = 0;
    int last;
    do {
        last = infl_bits(&s, 1);
        int type = infl_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        int rc;
        if (type == 0)      rc = infl_block_stored(&s);
        else if (type == 1) rc = infl_block_fixed(&s);
        else if (type == 2) rc = infl_block_dynamic(&s);
        else                return -1;
        if (rc != 0) return -1;
    } while (!last);
    return (long)s.outpos;
}

#endif /* _MAKAR_INFLATE_H */
