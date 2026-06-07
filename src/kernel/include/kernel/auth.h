#ifndef _KERNEL_AUTH_H
#define _KERNEL_AUTH_H

#include <stdint.h>
#include <stddef.h>

/* microhash.c: 64-bit hash of arbitrary bytes (non-cryptographic, Arawn Davies). */
uint64_t microhash_u64(const uint8_t *data, size_t len);

/* Fills out[17] with 16 lowercase hex chars + NUL — the hash of data[0..len). */
void microhash_hex16(const uint8_t *data, size_t len, char out[17]);

/* shadow.c: verify or update a user's password in /etc/shadow.
 * Returns 0 on success, -1 on failure. */
int shadow_verify      (const char *username, const char *password);
int shadow_set_password(const char *username, const char *password);

/* Full-screen TUI to change `user`'s password (login-style New/Confirm form).
 * Caller must gate this to installed systems (writable rootfs).  0 ok / -1. */
int passwd_screen(const char *user);

/* Ctrl-Alt-Del menu (text/shell session): Log off or Change password.
 * Runs in the caller's task context (Log off task_exit()s it). */
void cad_menu(void);

/* Returns 1 if `username` has an entry in /etc/shadow, else 0. */
int shadow_user_exists (const char *username);

/* login.c: full-screen login prompt; returns when user is authenticated. */
void login_screen(void);

/* Returns the username that authenticated in the current session.
 * Defaults to "user" on live boots where login_screen was skipped. */
const char *auth_current_user(void);

/* Clear the session user (logout) -- shell_login_loop won't start a shell
 * until a login sets it again. */
void auth_clear_user(void);

/* Force the session user with NO password check.  Used by the rescue boot
 * (single-user mode): physical-console recovery is root-equivalent by design,
 * the same trust model as Linux `init=/bin/sh`.  Do not use for normal login. */
void auth_force_user(const char *user);

/* Verify credentials and, on success, set the session user.  Returns 0 on
 * success, -1 on failure.  Backs SYS_LOGIN for the ring-3 GUI login screen. */
int auth_login(const char *username, const char *password);

/* Re-enter the login prompt (call from `logout` shell command). */
void auth_logout(void);

/* Attempt auto-login, skipping the password prompt.  The user is taken from
 * `cmdline_user` (the `autologin=<user>` kernel arg) when non-NULL/non-empty,
 * otherwise from the first token of /etc/autologin.  Returns 1 and records the
 * session user (auth_current_user) when the resolved user exists in
 * /etc/shadow; returns 0 (so the caller shows login_screen) when no autologin
 * is configured or the named user is invalid. */
int auth_try_autologin(const char *cmdline_user);

#endif /* _KERNEL_AUTH_H */
