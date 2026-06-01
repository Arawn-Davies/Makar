/* shadow.c -- /etc/shadow read/write and salt generation (not yet implemented).
 *
 * Planned format (subset of Linux shadow):
 *   username:$6$<salt>$<sha256-hash>:::::::
 *
 * Salt: 16 random printable chars derived from PIT tick counter + RTC entropy.
 * Hash: SHA-256(salt + password), stored as 64-char hex.
 * /etc/passwd: username:x:uid:gid::/home/username:/apps/sh.elf
 */
#include "auth.h"

int shadow_verify(const char *username, const char *password)
{
    (void)username; (void)password;
    /* TODO: read /etc/shadow, extract salt, hash input, compare */
    return 0;
}

int shadow_set_password(const char *username, const char *password)
{
    (void)username; (void)password;
    /* TODO: generate salt, hash, write /etc/shadow entry */
    return 0;
}
