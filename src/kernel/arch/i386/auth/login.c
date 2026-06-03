/* login.c - Full-screen login prompt with installer-style TUI.
 *
 * Appearance mirrors the installer's colour scheme (deep-blue backdrop,
 * cyan title bar, white text) and the Medli login flow (username → masked
 * password, 3-attempt lockout).
 *
 * login_screen() blocks until a user authenticates successfully, then
 * returns so the caller (kernel_main / shell task) can proceed.
 *
 * logout() is the user-facing complement: clears the screen and re-invokes
 * login_screen() so the session is handed back to the login prompt.
 */

#include <kernel/auth.h>
#include <kernel/vfs.h>
#include <kernel/vesa_tty.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/timer.h>
#include <kernel/serial.h>
#include <kernel/version.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* Installer palette (replicated here so auth/ has no dep on installer.h) */
#define C_BG     0x00102040u   /* deep blue backdrop  */
#define C_FG     0x00FFFFFFu   /* white text          */
#define C_DIM    0x00A0B0C0u   /* muted / hint text   */
#define C_TITLE  0x0040E0FFu   /* cyan title bar      */
#define C_SELFG  0x00102040u   /* title text (dark)   */
#define C_WARN   0x00FF6060u   /* red warning/error   */
#define C_OK     0x0060FF80u   /* green success       */
#define C_INPUT  0x00FFFF88u   /* yellow input text   */

#define MAX_ATTEMPTS 3

/* --------------------------------------------------------------------------
 * Session state
 * --------------------------------------------------------------------------*/
static char s_current_user[64] = "user";

const char *auth_current_user(void) { return s_current_user; }

/* --------------------------------------------------------------------------
 * Geometry helpers (same pattern as installer.c)
 * --------------------------------------------------------------------------*/
static int      s_gui;
static uint32_t s_cols, s_rows;

static void login_geometry(void)
{
    s_gui = vesa_tty_is_ready();
    if (s_gui) {
        s_cols = vesa_tty_get_cols();
        s_rows = vesa_tty_usable_rows();
    } else {
        s_cols = 80;
        s_rows = 24;
    }
}

/* --------------------------------------------------------------------------
 * Paint helpers
 * --------------------------------------------------------------------------*/
static void paint_fill(uint32_t bg)
{
    if (!s_gui) return;
    char blank[256];
    uint32_t n = (s_cols < 255) ? s_cols : 255;
    for (uint32_t i = 0; i < n; i++) blank[i] = ' ';
    blank[n] = '\0';
    for (uint32_t r = 0; r < s_rows; r++)
        vesa_tty_paint_string_at(0, r, blank, C_FG, bg);
}

static void tui_at(uint32_t col, uint32_t row, const char *s,
                   uint32_t fg, uint32_t bg)
{
    if (s_gui) vesa_tty_paint_string_at(col, row, s, fg, bg);
}

static void tui_center(uint32_t row, const char *s, uint32_t fg, uint32_t bg)
{
    size_t len = strlen(s);
    uint32_t col = (len < s_cols) ? (uint32_t)((s_cols - len) / 2) : 0;
    tui_at(col, row, s, fg, bg);
}

/* Fill a full row with spaces in the given colour, then paint the string. */
static void tui_row(uint32_t row, const char *s, uint32_t fg, uint32_t bg)
{
    if (!s_gui) return;
    char blank[256];
    uint32_t n = (s_cols < 255) ? s_cols : 255;
    for (uint32_t i = 0; i < n; i++) blank[i] = ' ';
    blank[n] = '\0';
    vesa_tty_paint_string_at(0, row, blank, fg, bg);
    if (s) tui_center(row, s, fg, bg);
}

/* Draw the standard header: cyan title bar row + dim hint at bottom. */
static void login_frame(const char *hint)
{
    paint_fill(C_BG);

    /* Title bar */
    char title[128];
    const char *ver = MAKAR_VERSION;
    size_t vlen = strlen(ver);
    size_t tlen = 6 + 1 + vlen;  /* "Makar " + version */
    if (tlen < sizeof(title) - 1) {
        size_t p = 0;
        const char *prefix = "Makar ";
        while (*prefix) title[p++] = *prefix++;
        for (size_t i = 0; i < vlen; i++) title[p++] = ver[i];
        title[p] = '\0';
    } else {
        const char *fb = "Makar Login";
        size_t p = 0;
        while (*fb) title[p++] = *fb++;
        title[p] = '\0';
    }
    tui_row(0, title, C_SELFG, C_TITLE);

    /* Subtitle */
    tui_center(2, "Login", C_FG, C_BG);

    /* Footer hint */
    if (hint)
        tui_at(2, s_rows - 1, hint, C_DIM, C_BG);
}

/* --------------------------------------------------------------------------
 * Masked line-read: echoes '*' per character.  Backspace erases the last
 * star in GUI mode (overwrite with space) or via VGA t_backspace().
 * --------------------------------------------------------------------------*/
static void read_masked(char *buf, size_t max,
                        uint32_t col, uint32_t row,
                        uint32_t field_width)
{
    (void)field_width;
    size_t len = 0;

    while (1) {
        unsigned char c = keyboard_getchar();
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (len) {
                len--;
                if (s_gui)
                    vesa_tty_paint_string_at(col + (uint32_t)len, row, " ", C_FG, C_BG);
                else
                    t_backspace();
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) {
            buf[len++] = (char)c;
            if (s_gui)
                vesa_tty_paint_string_at(col + (uint32_t)(len - 1), row, "*", C_FG, C_BG);
            else
                t_putchar('*');
        }
    }
    buf[len] = '\0';
}

/* Plain (unmasked) line-read with GUI support. */
static void read_plain(char *buf, size_t max,
                       uint32_t col, uint32_t row,
                       uint32_t field_width)
{
    size_t len = 0;
    (void)field_width;

    while (1) {
        unsigned char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            if (!s_gui) t_putchar('\n');
            break;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                if (s_gui) {
                    vesa_tty_paint_string_at(col + (uint32_t)len, row,
                                            " ", C_FG, C_BG);
                } else {
                    t_backspace();
                }
            }
            continue;
        }

        if (c < 0x20 || c > 0x7E) continue;

        if (len < max - 1) {
            buf[len++] = (char)c;
            char ch2[2] = { (char)c, '\0' };
            if (s_gui) {
                vesa_tty_paint_string_at(col + (uint32_t)(len - 1), row,
                                        ch2, C_INPUT, C_BG);
            } else {
                t_putchar((char)c);
            }
        }
    }
    buf[len] = '\0';
}

/* --------------------------------------------------------------------------
 * login_screen
 * --------------------------------------------------------------------------*/
void login_screen(void)
{
    login_geometry();

    char username[64];
    char password[256];

    int attempts = 0;

    while (1) {
        /* Draw the frame */
        login_frame("Enter your username and password to log in");

        uint32_t mid_col  = s_cols / 2;
        uint32_t mid_row  = s_rows / 2;

        /* Box layout (centred) */
        uint32_t box_w    = 36;
        uint32_t box_left = (mid_col > box_w / 2) ? mid_col - box_w / 2 : 2;
        uint32_t label_col = box_left;
        uint32_t field_col = box_left + 12;  /* "Username:   " = 12 chars */

        uint32_t row_user  = mid_row - 2;
        uint32_t row_pass  = mid_row;
        uint32_t row_hint  = mid_row + 2;
        uint32_t row_err   = mid_row + 4;

        if (s_gui) {
            tui_at(label_col, row_user, "Username:   ", C_FG, C_BG);
            tui_at(label_col, row_pass, "Password:   ", C_FG, C_BG);
        } else {
            /* VGA: simple prompt flow */
            t_writestring("Username: ");
        }

        /* --- Read username --- */
        username[0] = '\0';
        if (s_gui) {
            /* Clear the field area first */
            char blank[32];
            for (int i = 0; i < 20; i++) blank[i] = ' ';
            blank[20] = '\0';
            vesa_tty_paint_string_at(field_col, row_user, blank, C_FG, C_BG);
            vesa_tty_set_cursor(field_col, row_user);
        }
        read_plain(username, sizeof(username), field_col, row_user, 20);

        if (!s_gui) t_writestring("Password: ");

        /* --- Read password --- */
        password[0] = '\0';
        if (s_gui) {
            char blank[32];
            for (int i = 0; i < 20; i++) blank[i] = ' ';
            blank[20] = '\0';
            vesa_tty_paint_string_at(field_col, row_pass, blank, C_FG, C_BG);
            vesa_tty_set_cursor(field_col, row_pass);

            /* Show hint label */
            tui_at(label_col, row_hint,
                   "Press Enter to log in", C_DIM, C_BG);
        }
        read_masked(password, sizeof(password), field_col, row_pass, 20);

        /* --- Verify --- */
        int ok = (shadow_verify(username, password) == 0);

        if (ok) {
            /* Record who logged in for auth_current_user(). */
            {
                size_t i = 0;
                while (username[i] && i < sizeof(s_current_user) - 1)
                    { s_current_user[i] = username[i]; i++; }
                s_current_user[i] = '\0';
            }
            if (s_gui) {
                tui_at(label_col, row_err,
                       "Login successful.        ", C_OK, C_BG);
                ksleep(60);  /* 0.6 s at 100 Hz */
            } else {
                t_writestring("Login successful.\n");
            }
            Serial_WriteString("[auth] login ok: ");
            Serial_WriteString(username);
            Serial_WriteString("\n");
            return;
        }

        attempts++;
        Serial_WriteString("[auth] login failed: ");
        Serial_WriteString(username);
        Serial_WriteString("\n");

        if (attempts >= MAX_ATTEMPTS) {
            if (s_gui) {
                tui_at(label_col, row_err,
                       "Too many failures. Halted.", C_WARN, C_BG);
            } else {
                t_writestring("Too many login failures.\n");
            }
            Serial_WriteString("[auth] locked out after 3 failures\n");
            /* Halt the system rather than continue without auth */
            while (1) __asm__ volatile("hlt");
        }

        /* Show failure message and loop */
        if (s_gui) {
            char msg[64] = "Incorrect credentials. Attempt X/3";
            msg[32] = (char)('0' + attempts);
            tui_at(label_col, row_err, msg, C_WARN, C_BG);
            ksleep(100);  /* 1 s pause */
        } else {
            t_writestring("Incorrect credentials.\n");
            ksleep(100);
        }
    }
}

/* --------------------------------------------------------------------------
 * logout: re-enter the login screen (call from shell `logout` command).
 * --------------------------------------------------------------------------*/
void auth_logout(void)
{
    Serial_WriteString("[auth] logout\n");
    login_screen();
}

/* --------------------------------------------------------------------------
 * auth_try_autologin: resolve an auto-login user (cmdline arg first, then
 * /etc/autologin) and, if it exists in /etc/shadow, sign in without a
 * password.  Returns 0 to fall back to the interactive login prompt.
 * --------------------------------------------------------------------------*/
int auth_try_autologin(const char *cmdline_user)
{
    char namebuf[64];
    const char *user = NULL;

    if (cmdline_user && *cmdline_user) {
        user = cmdline_user;
    } else {
        /* First whitespace-delimited token of /etc/autologin. */
        char fbuf[80];
        uint32_t sz = 0;
        if (vfs_read_file("/etc/autologin", fbuf, sizeof(fbuf) - 1, &sz) == 0 && sz > 0) {
            fbuf[sz] = '\0';
            const char *p = fbuf;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            size_t i = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
                   i < sizeof(namebuf) - 1)
                namebuf[i++] = *p++;
            namebuf[i] = '\0';
            if (namebuf[0]) user = namebuf;
        }
    }

    if (!user || !*user)
        return 0;                       /* no autologin configured */

    if (!shadow_user_exists(user)) {
        Serial_WriteString("[auth] autologin user not found, requiring login: ");
        Serial_WriteString((char *)user);
        Serial_WriteString("\n");
        return 0;                       /* invalid -> fall back to prompt */
    }

    size_t i = 0;
    while (user[i] && i < sizeof(s_current_user) - 1) { s_current_user[i] = user[i]; i++; }
    s_current_user[i] = '\0';
    Serial_WriteString("[auth] autologin: ");
    Serial_WriteString(s_current_user);
    Serial_WriteString("\n");
    return 1;
}
