#ifndef _KERNEL_AUTH_H
#define _KERNEL_AUTH_H

/* SHA-256: fills out[65] with lowercase hex digest of input. */
void sha256_hex(const char *input, char out[65]);

/* shadow.c: verify or update a user's password in /etc/shadow. */
int shadow_verify    (const char *username, const char *password);
int shadow_set_password(const char *username, const char *password);

/* login.c: full-screen login prompt; returns when user authenticated. */
void login_screen(void);

#endif
