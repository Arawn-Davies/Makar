/*
 * inflate.c -- raw DEFLATE (RFC 1951) decompressor.
 *
 * A compact, allocation-free inflate sufficient to extract ZIP method-8
 * entries.  Canonical-Huffman decode follows the classic puff/tinf structure
 * (Mark Adler's puff): per-length code counts + a symbol table sorted by code.
 * Stored (type 0), fixed (type 1) and dynamic (type 2) blocks are supported.
 */
#include <kernel/inflate.h>
#include <string.h>

#define INF_MAXBITS 15
#define INF_MAXLCODES 286
#define INF_MAXDCODES 30
#define INF_MAXCODES (INF_MAXLCODES + INF_MAXDCODES)

typedef struct {
    const uint8_t *src;
    uint32_t src_len, src_pos;
    uint32_t bitbuf, bitcnt;
    uint8_t  *dst;
    uint32_t dst_cap, dst_pos;
    int error;
} inf_state;

typedef struct {
    short count[INF_MAXBITS + 1];
    short symbol[INF_MAXCODES];
} inf_huff;

static int inf_getbit(inf_state *s)
{
    if (s->bitcnt == 0) {
        if (s->src_pos >= s->src_len) { s->error = 1; return 0; }
        s->bitbuf = s->src[s->src_pos++];
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1u);
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static uint32_t inf_getbits(inf_state *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)inf_getbit(s) << i;
    return v;
}

static void inf_build(inf_huff *h, const uint8_t *lengths, int n)
{
    short offs[INF_MAXBITS + 1];
    int len, sym;
    for (len = 0; len <= INF_MAXBITS; len++) h->count[len] = 0;
    for (sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (len = 1; len < INF_MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (short)sym;
}

static int inf_decode(inf_state *s, const inf_huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= INF_MAXBITS; len++) {
        code |= inf_getbit(s);
        int count = h->count[len];
        if (code - first < count)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static const short inf_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short inf_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short inf_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short inf_dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inf_block(inf_state *s, const inf_huff *lh, const inf_huff *dh)
{
    for (;;) {
        int sym = inf_decode(s, lh);
        if (s->error) return -1;
        if (sym == 256) return 0;            /* end of block */
        if (sym < 256) {
            if (s->dst_pos >= s->dst_cap) { s->error = 1; return -1; }
            s->dst[s->dst_pos++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) { s->error = 1; return -1; }
            int len = inf_len_base[sym] + (int)inf_getbits(s, inf_len_extra[sym]);
            int dsym = inf_decode(s, dh);
            if (s->error || dsym < 0 || dsym >= 30) { s->error = 1; return -1; }
            int dist = inf_dist_base[dsym] + (int)inf_getbits(s, inf_dist_extra[dsym]);
            if ((uint32_t)dist > s->dst_pos) { s->error = 1; return -1; }
            if (s->dst_pos + (uint32_t)len > s->dst_cap) { s->error = 1; return -1; }
            for (int i = 0; i < len; i++) {
                s->dst[s->dst_pos] = s->dst[s->dst_pos - dist];
                s->dst_pos++;
            }
        }
    }
}

static void inf_fixed(inf_huff *lh, inf_huff *dh)
{
    uint8_t lengths[288];
    int i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (;        i < 256; i++) lengths[i] = 9;
    for (;        i < 280; i++) lengths[i] = 7;
    for (;        i < 288; i++) lengths[i] = 8;
    inf_build(lh, lengths, 288);
    for (i = 0;   i < 30;  i++) lengths[i] = 5;
    inf_build(dh, lengths, 30);
}

static const uint8_t inf_clc_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

static int inf_dynamic(inf_state *s, inf_huff *lh, inf_huff *dh)
{
    uint8_t lengths[INF_MAXCODES];
    int hlit  = (int)inf_getbits(s, 5) + 257;
    int hdist = (int)inf_getbits(s, 5) + 1;
    int hclen = (int)inf_getbits(s, 4) + 4;
    if (hlit > INF_MAXLCODES || hdist > INF_MAXDCODES) { s->error = 1; return -1; }

    uint8_t clen[19];
    int i;
    for (i = 0; i < 19; i++) clen[i] = 0;
    for (i = 0; i < hclen; i++) clen[inf_clc_order[i]] = (uint8_t)inf_getbits(s, 3);

    inf_huff clh;
    inf_build(&clh, clen, 19);

    int num = 0;
    while (num < hlit + hdist) {
        int sym = inf_decode(s, &clh);
        if (s->error) return -1;
        if (sym < 16) {
            lengths[num++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (num == 0) { s->error = 1; return -1; }
            uint8_t prev = lengths[num - 1];
            int rep = 3 + (int)inf_getbits(s, 2);
            while (rep-- && num < hlit + hdist) lengths[num++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)inf_getbits(s, 3);
            while (rep-- && num < hlit + hdist) lengths[num++] = 0;
        } else {                              /* sym == 18 */
            int rep = 11 + (int)inf_getbits(s, 7);
            while (rep-- && num < hlit + hdist) lengths[num++] = 0;
        }
    }
    if (s->error) return -1;
    inf_build(lh, lengths, hlit);
    inf_build(dh, lengths + hlit, hdist);
    return 0;
}

static int inf_stored(inf_state *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;                            /* align to byte boundary */
    if (s->src_pos + 4 > s->src_len) { s->error = 1; return -1; }
    uint32_t len = (uint32_t)s->src[s->src_pos] |
                   ((uint32_t)s->src[s->src_pos + 1] << 8);
    s->src_pos += 4;                          /* skip LEN + ~LEN */
    if (s->src_pos + len > s->src_len) { s->error = 1; return -1; }
    if (s->dst_pos + len > s->dst_cap) { s->error = 1; return -1; }
    memcpy(s->dst + s->dst_pos, s->src + s->src_pos, len);
    s->dst_pos += len;
    s->src_pos += len;
    return 0;
}

int inflate_raw(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *out_len)
{
    inf_state s;
    memset(&s, 0, sizeof s);
    s.src = src; s.src_len = src_len;
    s.dst = dst; s.dst_cap = dst_cap;

    int final;
    do {
        final = inf_getbit(&s);
        int type = (int)inf_getbits(&s, 2);
        if (s.error) return -1;
        if (type == 0) {
            if (inf_stored(&s) != 0) return -1;
        } else if (type == 1) {
            inf_huff lh, dh;
            inf_fixed(&lh, &dh);
            if (inf_block(&s, &lh, &dh) != 0) return -1;
        } else if (type == 2) {
            inf_huff lh, dh;
            if (inf_dynamic(&s, &lh, &dh) != 0) return -1;
            if (inf_block(&s, &lh, &dh) != 0) return -1;
        } else {
            return -1;                        /* reserved block type */
        }
    } while (!final && !s.error);

    if (s.error) return -1;
    if (out_len) *out_len = s.dst_pos;
    return 0;
}
