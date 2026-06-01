/* microhash.c - C port of the microhash 64-bit hash algorithm.
 *
 * Original: ~/src/microhash (C++ header-only, Arawn Davies).
 * Spec: https://arawn-davies.github.io/microhash/Specification
 *
 * Algorithm: two 32-bit state words, 32-byte blocks (16 bytes actively mixed
 * per block), rotate-XOR-ADD mixing.  NOT cryptographic; suitable for
 * checksums, password salting (paired with a random salt), and hash tables.
 *
 * API: microhash_u64(data, len) -> uint64_t
 *      microhash_hex16(data, len, out17) -> fills out17 with 16 hex chars + NUL
 */

#include <kernel/auth.h>
#include <stdint.h>
#include <stddef.h>

static uint32_t rotl32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

uint64_t microhash_u64(const uint8_t *data, size_t len)
{
    uint32_t state[2] = { 0x243F6A88u, 0x85A308D3u };

    const int BS = 32;
    size_t padded = ((len + 5 + (size_t)(BS - 1)) / (size_t)BS) * (size_t)BS;

    uint8_t block[32];

    for (size_t off = 0; off < padded; off += (size_t)BS) {
        for (int i = 0; i < BS; i++) {
            size_t idx = off + (size_t)i;
            if (idx < len)
                block[i] = data[idx];
            else if (idx == len)
                block[i] = 0x80u;
            else if (idx >= padded - 4)
                block[i] = (uint8_t)((len >> (8 * (BS - 1 - i))) & 0xFFu);
            else
                block[i] = 0x00u;
        }

        for (int i = 0; i < 4; i++) {
            size_t base = (size_t)i * 4;
            uint32_t word = (uint32_t)block[base]
                          | ((uint32_t)block[base + 1] << 8)
                          | ((uint32_t)block[base + 2] << 16)
                          | ((uint32_t)block[base + 3] << 24);

            state[0] = rotl32(state[0] ^ word, 5) + state[1];
            state[1] = rotl32(state[1] + word, 11) ^ state[0];
        }
    }

    uint32_t final_val = state[0] ^ rotl32(state[1], 3);
    return ((uint64_t)final_val << 32) | (uint64_t)state[1];
}

void microhash_hex16(const uint8_t *data, size_t len, char out[17])
{
    uint64_t h = microhash_u64(data, len);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
        out[i] = hex[(h >> (60 - i * 4)) & 0xFu];
    out[16] = '\0';
}
