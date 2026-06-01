/* microhash.elf - userspace microhash utility (Arawn Davies' algorithm).
 *
 * Usage:
 *   microhash [string ...]    hash each argument as a UTF-8 string
 *   microhash                 read from stdin, hash the entire input
 *
 * Output: "<16-hex-hash>  <input>" (sha-sum compatible format)
 */
#include "syscall.h"

typedef unsigned int   u32;
typedef unsigned long long u64;
typedef unsigned char  u8;
typedef unsigned long  usize;

/* ---------- algorithm (mirrors kernel microhash.c) ---------- */

static u32 rotl32(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

static u64 microhash_u64(const u8 *data, usize len)
{
    u32 s0 = 0x243F6A88u, s1 = 0x85A308D3u;
    const int BS = 32;
    usize padded = ((len + 5 + (usize)(BS - 1)) / (usize)BS) * (usize)BS;
    u8 block[32];

    for (usize off = 0; off < padded; off += (usize)BS) {
        for (int i = 0; i < BS; i++) {
            usize idx = off + (usize)i;
            if (idx < len)              block[i] = data[idx];
            else if (idx == len)        block[i] = 0x80u;
            else if (idx >= padded - 4) block[i] = (u8)((len >> (8*(BS-1-i)))&0xFFu);
            else                        block[i] = 0x00u;
        }
        for (int i = 0; i < 4; i++) {
            usize b = (usize)i * 4;
            u32 w = (u32)block[b] | ((u32)block[b+1]<<8)
                                  | ((u32)block[b+2]<<16)
                                  | ((u32)block[b+3]<<24);
            s0 = rotl32(s0 ^ w, 5) + s1;
            s1 = rotl32(s1 + w, 11) ^ s0;
        }
    }
    u32 f = s0 ^ rotl32(s1, 3);
    return ((u64)f << 32) | (u64)s1;
}

/* ---------- output helpers ---------- */

static void write_str(const char *s)
{
    usize n = 0; while (s[n]) n++;
    sys_write(1, s, n);
}

static void write_hex16(u64 h)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 0; i < 16; i++) buf[i] = hex[(h >> (60 - i*4)) & 0xFu];
    buf[16] = '\0';
    write_str(buf);
}

/* ---------- stdin reader ---------- */

static u8 s_buf[65536];
static usize s_buf_len;

static void read_stdin(void)
{
    s_buf_len = 0;
    long n;
    while (s_buf_len < sizeof(s_buf) &&
           (n = sys_read(0, (char *)s_buf + s_buf_len,
                         sizeof(s_buf) - s_buf_len)) > 0)
        s_buf_len += (usize)n;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        /* stdin mode */
        read_stdin();
        write_hex16(microhash_u64(s_buf, s_buf_len));
        write_str("  -\n");
    } else {
        for (int i = 1; i < argc; i++) {
            usize len = 0; while (argv[i][len]) len++;
            write_hex16(microhash_u64((u8 *)argv[i], len));
            write_str("  ");
            write_str(argv[i]);
            write_str("\n");
        }
    }
    return 0;
}
