/* sha256.elf - userspace SHA-256 utility (RFC 6234, freestanding).
 *
 * Usage:
 *   sha256 [string ...]    hash each argument as a UTF-8 string
 *   sha256                 read from stdin, hash the entire input
 *
 * Output: "<64-hex-hash>  <input>" (sha256sum compatible format)
 */
#include "syscall.h"

typedef unsigned int  u32;
typedef unsigned char u8;
typedef unsigned long usize;
typedef unsigned long long u64;

/* ---------- SHA-256 (RFC 4634) ---------- */

#define ROR32(v,n) (((v)>>(n))|((v)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^((~(e))&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define S0(a) (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define S1(e) (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define s0(w) (ROR32(w,7)^ROR32(w,18)^((w)>>3))
#define s1(w) (ROR32(w,17)^ROR32(w,19)^((w)>>10))

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

typedef struct { u32 h[8]; u64 bitlen; u8 buf[64]; u32 buflen; } sha256_ctx;

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85;
    c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c;
    c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->bitlen=0; c->buflen=0;
}

static void sha256_block(sha256_ctx *c, const u8 *blk)
{
    u32 w[64]; int i;
    for (i=0;i<16;i++)
        w[i]=((u32)blk[i*4]<<24)|((u32)blk[i*4+1]<<16)|
             ((u32)blk[i*4+2]<<8)|(u32)blk[i*4+3];
    for (i=16;i<64;i++) w[i]=s1(w[i-2])+w[i-7]+s0(w[i-15])+w[i-16];

    u32 a=c->h[0],b=c->h[1],d=c->h[2],e2=c->h[3];
    u32 e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (i=0;i<64;i++) {
        u32 t1=h+S1(e)+CH(e,f,g)+K[i]+w[i];
        u32 t2=S0(a)+MAJ(a,b,d);
        h=g; g=f; f=e; e=e2+t1; e2=d; d=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e2;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_update(sha256_ctx *c, const u8 *data, usize len)
{
    while (len) {
        u32 room = 64 - c->buflen;
        u32 take = (len < room) ? (u32)len : room;
        for (u32 i=0;i<take;i++) c->buf[c->buflen+i]=data[i];
        c->buflen+=take; data+=take; len-=take;
        if (c->buflen==64) { sha256_block(c,c->buf); c->bitlen+=512; c->buflen=0; }
    }
}

static void sha256_final(sha256_ctx *c, u8 out[32])
{
    u64 bits = c->bitlen + (u64)c->buflen*8;
    c->buf[c->buflen++]=0x80;
    if (c->buflen>56) {
        while (c->buflen<64) c->buf[c->buflen++]=0;
        sha256_block(c,c->buf); c->buflen=0;
    }
    while (c->buflen<56) c->buf[c->buflen++]=0;
    /* Big-endian 64-bit bit count in bytes 56-63 */
    for (int i=0;i<8;i++) c->buf[56+i]=(u8)(bits>>(56-8*i));
    sha256_block(c,c->buf);
    for (int i=0;i<8;i++) {
        out[i*4  ]=(u8)(c->h[i]>>24); out[i*4+1]=(u8)(c->h[i]>>16);
        out[i*4+2]=(u8)(c->h[i]>>8);  out[i*4+3]=(u8)(c->h[i]);
    }
}

/* ---------- output helpers ---------- */

static void write_str(const char *s) { usize n=0; while(s[n])n++; sys_write(1,s,n); }

static void write_hex(const u8 *d, int n)
{
    static const char h[]="0123456789abcdef";
    char buf[3]; buf[2]='\0';
    for (int i=0;i<n;i++) { buf[0]=h[d[i]>>4]; buf[1]=h[d[i]&0xF]; write_str(buf); }
}

/* ---------- stdin reader ---------- */

static u8 s_buf[65536]; static usize s_buf_len;
static void read_stdin(void)
{
    s_buf_len=0; long n;
    while (s_buf_len<sizeof(s_buf) &&
           (n=sys_read(0,(char*)s_buf+s_buf_len,sizeof(s_buf)-s_buf_len))>0)
        s_buf_len+=(usize)n;
}

/* ---------- main ---------- */

static void hash_and_print(const u8 *data, usize len, const char *label)
{
    sha256_ctx c; u8 dig[32];
    sha256_init(&c); sha256_update(&c,data,len); sha256_final(&c,dig);
    write_hex(dig,32); write_str("  "); write_str(label); write_str("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        read_stdin();
        hash_and_print(s_buf, s_buf_len, "-");
    } else {
        for (int i=1;i<argc;i++) {
            usize len=0; while(argv[i][len])len++;
            hash_and_print((u8*)argv[i],len,argv[i]);
        }
    }
    return 0;
}
