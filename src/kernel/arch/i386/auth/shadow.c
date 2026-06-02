/* shadow.c - /etc/shadow password database: salted microhash storage.
 *
 * File format (one entry per line):
 *   username:$mh$<16-hex-salt>$<16-hex-hash>:::::::
 *
 * Salt: 8 bytes of entropy (PIT ticks XOR RTC unix time), hex-encoded.
 * Hash: microhash_u64(salt_bytes[8] ++ password_bytes), hex-encoded 16 chars.
 *
 * microhash is non-cryptographic (see auth.h).  The salt ensures two users
 * with the same password don't share a hash entry, which is the primary goal
 * for a hobby OS.  Use with a strong hash if you ever run this on real HW.
 *
 * First-boot: if /etc/shadow is missing, shadow_verify("root","") succeeds
 * (no-auth mode) so the login screen stays accessible before any password
 * has been set.  shadow_set_password creates the file on a writable rootfs.
 */

#include <kernel/auth.h>
#include <kernel/vfs.h>
#include <kernel/timer.h>
#include <kernel/rtc.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#define SHADOW_PATH  "/etc/shadow"
#define SHADOW_MAX   4096       /* max file size we'll read/write */
#define LINE_MAX     256
#define SALT_BYTES   8          /* 64 bits of entropy → 16 hex chars */

/* -------------------------------------------------------------------------
 * Salt generation
 * -------------------------------------------------------------------------
 * XOR PIT tick counter with RTC unix seconds, then microhash the 8-byte
 * concatenation to spread the bits further.  Result is 8 bytes → 16 hex.
 */
static void generate_salt(char salt_hex[17])
{
    uint32_t ticks = timer_get_ticks();
    uint32_t rtcsec = 0;
    rtc_unix_time(&rtcsec);

    /* 8-byte seed: ticks LE + rtcsec LE */
    uint8_t seed[8];
    seed[0] = (uint8_t)(ticks);
    seed[1] = (uint8_t)(ticks >> 8);
    seed[2] = (uint8_t)(ticks >> 16);
    seed[3] = (uint8_t)(ticks >> 24);
    seed[4] = (uint8_t)(rtcsec);
    seed[5] = (uint8_t)(rtcsec >> 8);
    seed[6] = (uint8_t)(rtcsec >> 16);
    seed[7] = (uint8_t)(rtcsec >> 24);

    /* Hash the seed itself to improve distribution */
    uint64_t h = microhash_u64(seed, 8);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
        salt_hex[i] = hex[(h >> (60 - i * 4)) & 0xFu];
    salt_hex[16] = '\0';
}

/* -------------------------------------------------------------------------
 * Hash: microhash_u64(salt_hex[16] ++ password)
 * -------------------------------------------------------------------------*/
static void compute_hash(const char salt_hex[16], const char *password,
                         char hash_hex[17])
{
    /* Concatenate salt hex string + password bytes into one buffer */
    size_t plen = strlen(password);
    size_t total = 16 + plen;
    /* Stack-safe: passwords are bounded in practice; 16+256 is fine. */
    if (total > 1024) total = 1024;
    uint8_t buf[1024];
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)salt_hex[i];
    for (size_t i = 0; i < plen && 16 + i < total; i++)
        buf[16 + i] = (uint8_t)password[i];
    microhash_hex16(buf, total, hash_hex);
}

/* -------------------------------------------------------------------------
 * Read /etc/shadow and find the entry for `username'.
 * Returns 1 if found, 0 if not found, -1 on I/O error.
 * On success, fills salt[17] and stored_hash[17].
 * -------------------------------------------------------------------------*/
static int shadow_find(const char *username, char salt[17], char stored[17])
{
    static char fbuf[SHADOW_MAX];
    uint32_t sz = 0;

    if (vfs_read_file(SHADOW_PATH, fbuf, sizeof(fbuf) - 1, &sz) != 0)
        return -1;
    fbuf[sz] = '\0';

    size_t ulen = strlen(username);
    char *line = fbuf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char saved = *end;
        *end = '\0';

        /* Match: "username:$mh$<salt16>$<hash16>:::::::..." */
        if (strncmp(line, username, ulen) == 0 && line[ulen] == ':') {
            char *p = line + ulen + 1;
            /* Expect $mh$ prefix */
            if (p[0] == '$' && p[1] == 'm' && p[2] == 'h' && p[3] == '$') {
                p += 4;
                for (int i = 0; i < 16; i++) salt[i] = p[i];
                salt[16] = '\0';
                p += 16;
                if (*p == '$') {
                    p++;
                    for (int i = 0; i < 16; i++) stored[i] = p[i];
                    stored[16] = '\0';
                    *end = saved;
                    return 1;
                }
            }
        }

        *end = saved;
        line = (*end == '\n') ? end + 1 : end;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

int shadow_user_exists(const char *username)
{
    char salt[17], stored[17];
    return shadow_find(username, salt, stored) == 1 ? 1 : 0;
}

int shadow_verify(const char *username, const char *password)
{
    char salt[17], stored[17];
    int rc = shadow_find(username, salt, stored);

    if (rc < 0) {
        /* No shadow file: first-boot / no-auth mode.  Only accept root
         * with an empty password so the system stays locked on real use. */
        if (strcmp(username, "root") == 0 && password[0] == '\0')
            return 0;
        return -1;
    }
    if (rc == 0)
        return -1;   /* user not found */

    char computed[17];
    compute_hash(salt, password, computed);
    return (strncmp(computed, stored, 16) == 0) ? 0 : -1;
}

int shadow_set_password(const char *username, const char *password)
{
    char salt[17];
    generate_salt(salt);

    char hash[17];
    compute_hash(salt, password, hash);

    /* Build the new entry line */
    /* "username:$mh$<salt16>$<hash16>::::::\n" */
    char entry[LINE_MAX];
    size_t ulen = strlen(username);
    if (ulen > 64) return -1;
    size_t pos = 0;
    for (size_t i = 0; i < ulen; i++) entry[pos++] = username[i];
    entry[pos++] = ':';
    entry[pos++] = '$'; entry[pos++] = 'm'; entry[pos++] = 'h'; entry[pos++] = '$';
    for (int i = 0; i < 16; i++) entry[pos++] = salt[i];
    entry[pos++] = '$';
    for (int i = 0; i < 16; i++) entry[pos++] = hash[i];
    /* Remaining fields are empty (passwd/shadow compat padding) */
    const char *tail = ":::::::";
    for (size_t i = 0; tail[i]; i++) entry[pos++] = tail[i];
    entry[pos++] = '\n';
    entry[pos] = '\0';

    /* Read existing file, remove old entry for this user, append new one */
    static char fbuf[SHADOW_MAX];
    uint32_t sz = 0;
    int exists = (vfs_read_file(SHADOW_PATH, fbuf, sizeof(fbuf) - 1, &sz) == 0);
    if (exists) fbuf[sz] = '\0'; else { fbuf[0] = '\0'; sz = 0; }

    /* Build output: copy every line that doesn't start with "username:" */
    static char outbuf[SHADOW_MAX];
    size_t outpos = 0;
    size_t ulen2 = strlen(username);
    char *line = fbuf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;
        size_t llen = (size_t)(end - line) + (*end == '\n' ? 1 : 0);
        int skip = (strncmp(line, username, ulen2) == 0 && line[ulen2] == ':');
        if (!skip && outpos + llen < sizeof(outbuf) - 1) {
            for (size_t i = 0; i < llen; i++) outbuf[outpos++] = line[i];
        }
        line = (*end == '\n') ? end + 1 : end;
    }

    /* Append new entry */
    size_t elen = pos;
    if (outpos + elen < sizeof(outbuf)) {
        for (size_t i = 0; i < elen; i++) outbuf[outpos++] = entry[i];
    }

    return vfs_write_file(SHADOW_PATH, outbuf, (uint32_t)outpos);
}
