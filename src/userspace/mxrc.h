/*
 * mxrc.h -- the per-user GUI profile, ~/.mxrc.
 *
 * A tiny "Key=value" config (one per line) shared by the window manager and its
 * clients: the desktop wallpaper, dock tray-visibility toggles, etc.  All access
 * goes through here so multiple writers (mximg writes Wallpaper, the WM writes
 * the tray toggles) coexist -- mxrc_set is read-modify-write and preserves every
 * other key.  Home is resolved from the logged-in user (root -> /root, else
 * /home/<user>).
 */
#ifndef MXRC_H
#define MXRC_H

/* Build ~/<suffix> for the logged-in user into out[0..cap). */
void mxrc_home(const char *suffix, char *out, int cap);

/* Read Key=value from ~/.mxrc.  Returns 0 and fills out on hit, -1 on miss. */
int  mxrc_get(const char *key, char *out, int cap);

/* Read an integer-valued key, or `def` if absent/unparsable. */
int  mxrc_get_int(const char *key, int def);

/* Set/replace Key=value in ~/.mxrc, preserving all other keys (read-modify-write). */
void mxrc_set(const char *key, const char *val);
void mxrc_set_int(const char *key, int val);

#endif /* MXRC_H */
