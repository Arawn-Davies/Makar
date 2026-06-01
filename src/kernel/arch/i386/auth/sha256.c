/* sha256.c -- SHA-256 for password hashing (not yet implemented).
 *
 * Port target: Medli/System/Framework/Crypto/SHA256.cs (261 lines, C#).
 * Standard RFC 6234 SHA-256; no dependencies beyond <stdint.h>.
 * Output: 64-char lowercase hex string in caller-supplied buf[65].
 */
#include "auth.h"

void sha256_hex(const char *input, char out[65])
{
    (void)input;
    /* TODO: implement */
    for (int i = 0; i < 64; i++) out[i] = '0';
    out[64] = '\0';
}
