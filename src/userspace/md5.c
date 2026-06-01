/* md5.elf - userspace MD5 utility (RFC 1321, freestanding).
 *
 * Usage:
 *   md5 [string ...]    hash each argument as a UTF-8 string
 *   md5                 read from stdin, hash the entire input
 *
 * Output: "<32-hex-hash>  <input>" (md5sum compatible format)
 */
#include "syscall.h"

typedef unsigned int  u32;
typedef unsigned char u8;
typedef unsigned long usize;

/* ---------- MD5 (RFC 1321) ---------- */

/* Precomputed T[i] = floor(2^32 * |sin(i+1)|) */
static const u32 T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
    0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
    0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
    0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
    0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
    0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const u32 S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

#define ROTL(v,n) (((v)<<(n))|((v)>>(32-(n))))

static u32 s_h[4];
static unsigned long long s_bits;
static u8 s_buf2[64];
static u32 s_buflen2;

static void md5_init(void)
{
    s_h[0]=0x67452301; s_h[1]=0xefcdab89;
    s_h[2]=0x98badcfe; s_h[3]=0x10325476;
    s_bits=0; s_buflen2=0;
}

static void md5_block(const u8 *blk)
{
    u32 M[16];
    for (int i=0;i<16;i++)
        M[i]=(u32)blk[i*4]|((u32)blk[i*4+1]<<8)|
             ((u32)blk[i*4+2]<<16)|((u32)blk[i*4+3]<<24);

    u32 a=s_h[0],b=s_h[1],c=s_h[2],d=s_h[3];
    for (int i=0;i<64;i++) {
        u32 F,g;
        if (i<16)      { F=(b&c)|(~b&d); g=(u32)i; }
        else if (i<32) { F=(d&b)|(~d&c); g=(u32)(5*i+1)%16; }
        else if (i<48) { F=b^c^d;        g=(u32)(3*i+5)%16; }
        else           { F=c^(b|(~d));   g=(u32)(7*i)%16; }
        u32 temp=d; d=c; c=b;
        b=b+ROTL(a+F+M[g]+T[i],S[i]);
        a=temp;
    }
    s_h[0]+=a; s_h[1]+=b; s_h[2]+=c; s_h[3]+=d;
}

static void md5_update(const u8 *data, usize len)
{
    while (len) {
        u32 room=64-s_buflen2;
        u32 take=(len<room)?(u32)len:room;
        for (u32 i=0;i<take;i++) s_buf2[s_buflen2+i]=data[i];
        s_buflen2+=take; data+=take; len-=take;
        if (s_buflen2==64) { md5_block(s_buf2); s_bits+=512; s_buflen2=0; }
    }
}

static void md5_final(u8 out[16])
{
    unsigned long long bits = s_bits + (unsigned long long)s_buflen2*8;
    s_buf2[s_buflen2++]=0x80;
    if (s_buflen2>56) {
        while (s_buflen2<64) s_buf2[s_buflen2++]=0;
        md5_block(s_buf2); s_buflen2=0;
    }
    while (s_buflen2<56) s_buf2[s_buflen2++]=0;
    for (int i=0;i<8;i++) s_buf2[56+i]=(u8)(bits>>(8*i));
    md5_block(s_buf2);
    for (int i=0;i<4;i++) {
        out[i*4  ]=(u8)(s_h[i]);      out[i*4+1]=(u8)(s_h[i]>>8);
        out[i*4+2]=(u8)(s_h[i]>>16);  out[i*4+3]=(u8)(s_h[i]>>24);
    }
}

/* ---------- helpers ---------- */

static void write_str(const char *s) { usize n=0; while(s[n])n++; sys_write(1,s,n); }

static void write_hex(const u8 *d, int n)
{
    static const char h[]="0123456789abcdef";
    char buf[3]; buf[2]='\0';
    for (int i=0;i<n;i++) { buf[0]=h[d[i]>>4]; buf[1]=h[d[i]&0xF]; write_str(buf); }
}

static u8 s_in[65536]; static usize s_inlen;
static void read_stdin(void)
{
    s_inlen=0; long n;
    while (s_inlen<sizeof(s_in) &&
           (n=sys_read(0,(char*)s_in+s_inlen,sizeof(s_in)-s_inlen))>0)
        s_inlen+=(usize)n;
}

static void hash_and_print(const u8 *data, usize len, const char *label)
{
    u8 dig[16];
    md5_init(); md5_update(data,len); md5_final(dig);
    write_hex(dig,16); write_str("  "); write_str(label); write_str("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        read_stdin();
        hash_and_print(s_in, s_inlen, "-");
    } else {
        for (int i=1;i<argc;i++) {
            usize len=0; while(argv[i][len])len++;
            hash_and_print((u8*)argv[i],len,argv[i]);
        }
    }
    return 0;
}
