/*
 * shell_priv.h -- internal declarations shared between shell source files.
 *
 * Not part of the public kernel API; include only from shell*.c files.
 */

#ifndef SHELL_PRIV_H
#define SHELL_PRIV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHELL_MAX_INPUT  256
#define SHELL_MAX_ARGS   16   /* enough headroom for glob expansion */

/* Medli-compatible identity: username and hostname shown in the prompt. */
#define SHELL_USERNAME   "root"
#define SHELL_HOSTNAME   "makar"

/* Shell version - independent of the kernel version (<kernel/version.h>).
 * The shell is conceptually a separate component; bump this when shell
 * features change, MAKAR_VERSION when the kernel itself changes. */
#define SHELL_VERSION    "0.5.0"

#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__
#define COPYRIGHT  "Copyright (C) 2026 Arawn Davies"

/*
 * Medli-compatible colour scheme: white text on blue background.
 *
 * SHELL_COLOR_VGA  – VGA attribute byte: fg=15 (WHITE) | bg=1 (BLUE) << 4
 * SHELL_FG_RGB     – 24-bit RGB for the VESA framebuffer (foreground)
 * SHELL_BG_RGB     – 24-bit RGB for the VESA framebuffer (background)
 * SHELL_ERROR_RGB_PAIR – VGA attr for error text: fg=12 (LIGHT_RED) | bg=1 (BLUE) << 4
 */
#define SHELL_COLOR_VGA       0x1F   /* make_color(COLOR_WHITE,      COLOR_BLUE) */
#define SHELL_ERROR_COLOR_VGA 0x1C   /* make_color(COLOR_LIGHT_RED,  COLOR_BLUE) */
#define SHELL_FG_RGB          0xFFFFFF
#define SHELL_BG_RGB          0x0000AA

/* Per-VT colour scheme.  Each shell slot has its own palette so the
 * four TTYs are visually distinct -- helpful for orienting after a
 * VT switch.  shell_apply_scheme_for_tty() reads this table and
 * pushes both the VGA attribute byte and the VESA RGB pair into the
 * renderers.  shell_clear_screen routes through the same helper so
 * fullscreen apps (vix, maktop) restore the right palette on exit. */
typedef struct {
    uint8_t  vga;     /* 8-bit VGA attribute = fg | (bg << 4)     */
    uint32_t fg_rgb;  /* 24-bit VESA foreground                    */
    uint32_t bg_rgb;  /* 24-bit VESA background                    */
} shell_scheme_t;

/* Indexed by VT slot 0..3.  Tracks the user's stated preferences:
 *   VT0  light green on black
 *   VT1  white on black
 *   VT2  white on blue (the classic Makar look, preserved as default
 *                       for the most-used secondary VT)
 *   VT3  black on white                                            */
static const shell_scheme_t SHELL_SCHEMES[4] = {
    { 0x0A, 0x55FF55, 0x000000 },
    { 0x0F, 0xFFFFFF, 0x000000 },
    { 0x1F, 0xFFFFFF, 0x0000AA },
    { 0xF0, 0x000000, 0xFFFFFF },
};

/* Apply the scheme for VT slot `tty` to both renderers.  Out-of-range
 * tty falls back to slot 0. */
void shell_apply_scheme_for_tty(int tty);

/*
 * Command entry: all handlers share the same (int argc, char **argv) signature
 * for uniform dispatch.  A NULL name field terminates each module's table.
 *
 * fullscreen: set on handlers that paint over the framebuffer directly
 * (VIX, future curses-style apps).  shell_dispatch repaints the focused
 * VT's backing grid after such commands return so the shell's history
 * comes back without waiting for the next keystroke.
 */
typedef struct {
    const char *name;
    void (*fn)(int argc, char **argv);
    unsigned char fullscreen;
} shell_cmd_entry_t;

/* Command module tables (NULL-name terminated). */
extern const shell_cmd_entry_t help_cmds[];
extern const shell_cmd_entry_t display_cmds[];
extern const shell_cmd_entry_t system_cmds[];
extern const shell_cmd_entry_t disk_cmds[];
extern const shell_cmd_entry_t fs_cmds[];
extern const shell_cmd_entry_t apps_cmds[];
extern const shell_cmd_entry_t man_cmds[];

/* shell.c – REPL core */
void shell_readline(char *buf, size_t max);

/* shell_glob.c – wildcard (* and ?) expansion of argv tokens via VFS. */
int  shell_expand_globs(int argc, char **argv, int argv_cap,
                        char *storage, size_t storage_size);

/* shell_cmd_apps.c – shared ELF launcher used by exec and PATH lookup */
void shell_exec_elf(const char *path, int argc, char **argv);

/* Shared helper: parse decimal or 0x-prefixed hex string to uint32. */
static inline uint32_t parse_uint(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        uint32_t v = 0;
        s += 2;
        while (*s) {
            char c = *s++;
            if (c >= '0' && c <= '9')      v = v * 16 + (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
            else break;
        }
        return v;
    }
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

#endif /* SHELL_PRIV_H */
