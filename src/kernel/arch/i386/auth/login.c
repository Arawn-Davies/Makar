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
#include <kernel/task.h>
#include <kernel/vtty.h>
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

/* Clear the session user (logout).  shell_login_loop refuses to start a shell
 * while this is empty, forcing a fresh login_screen() first. */
void auth_clear_user(void) { s_current_user[0] = '\0'; }

/* Verify + sign in (ring-3 GUI login via SYS_LOGIN).  Returns 0 / -1. */
int auth_login(const char *username, const char *password)
{
    if (!username || !password) return -1;
    if (shadow_verify(username, password) != 0) {
        Serial_WriteString("[auth] gui login failed: ");
        Serial_WriteString((char *)username);
        Serial_WriteString("\n");
        return -1;
    }
    size_t i = 0;
    while (username[i] && i < sizeof(s_current_user) - 1) { s_current_user[i] = username[i]; i++; }
    s_current_user[i] = '\0';
    Serial_WriteString("[auth] gui login ok: ");
    Serial_WriteString(s_current_user);
    Serial_WriteString("\n");
    return 0;
}

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
/* Edit one form field in place, resuming from its current contents.  Renders
 * the field + cursor, then reads keys until Tab or Enter.
 * Returns 1 on Enter, 0 on Tab.  `masked` shows '*' instead of the chars. */
static int edit_field(char *buf, size_t *plen, size_t max,
                      uint32_t col, uint32_t row, int masked)
{
    size_t len = *plen;
    if (s_gui) {
        char blank[32];
        for (int i = 0; i < 20; i++) blank[i] = ' ';
        blank[20] = '\0';
        vesa_tty_paint_string_at(col, row, blank, C_FG, C_BG);
        for (size_t i = 0; i < len && i < 20; i++) {
            char ch[2] = { masked ? '*' : buf[i], '\0' };
            vesa_tty_paint_string_at(col + (uint32_t)i, row, ch,
                                     masked ? C_FG : C_INPUT, C_BG);
        }
        vesa_tty_set_cursor(col + (uint32_t)len, row);
    }
    for (;;) {
        unsigned char c = keyboard_getchar();
        if (c == '\t')              { buf[len] = '\0'; *plen = len; return 0; }
        if (c == '\n' || c == '\r') { buf[len] = '\0'; *plen = len; return 1; }
        if (c == '\b' || c == 127) {
            if (len) {
                len--;
                if (s_gui) {
                    vesa_tty_paint_string_at(col + (uint32_t)len, row, " ", C_FG, C_BG);
                    vesa_tty_set_cursor(col + (uint32_t)len, row);
                } else t_backspace();
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        if (len < max - 1) {
            buf[len++] = (char)c;
            if (s_gui) {
                char ch[2] = { masked ? '*' : (char)c, '\0' };
                vesa_tty_paint_string_at(col + (uint32_t)(len - 1), row, ch,
                                         masked ? C_FG : C_INPUT, C_BG);
                vesa_tty_set_cursor(col + (uint32_t)len, row);
            } else t_putchar(masked ? '*' : (char)c);
        }
    }
}

void login_screen(void)
{
    /* The login screen owns the whole screen -- hide the shell status bar. */
    vesa_tty_set_status_visible(0);
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

        if (s_gui)
            tui_at(label_col, row_hint,
                   "Tab switches fields, Enter logs in", C_DIM, C_BG);

        /* --- Two-field form: Tab switches user<->pass, Enter advances /
         *     submits.  In VGA mode it degrades to sequential entry. --- */
        username[0] = '\0'; password[0] = '\0';
        size_t ulen = 0, plen = 0;
        if (!s_gui) t_writestring("Password: ");
        int field = 0;
        for (;;) {
            if (field == 0) {
                edit_field(username, &ulen, sizeof(username), field_col, row_user, 0);
                field = 1;                       /* Tab or Enter -> password   */
            } else {
                int r = edit_field(password, &plen, sizeof(password), field_col, row_pass, 1);
                if (r == 1) break;               /* Enter on password = submit */
                field = 0;                        /* Tab on password -> username */
            }
        }

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
            vesa_tty_set_status_visible(1);   /* restore the shell status bar */
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
 * cad_menu: Ctrl-Alt-Del menu (text/shell sessions only).  Two choices --
 * Log off (ends the current session) or Change password (TUI).  Invoked from
 * the keyboard layer when the focused text session is reading keys.  Esc
 * cancels.  Runs in the calling task's context, so "Log off" task_exit()s it.
 * --------------------------------------------------------------------------*/
void cad_menu(void)
{
    login_geometry();
    int prev_status = vesa_tty_status_enabled();
    vesa_tty_set_status_visible(0);

    int sel = 0;   /* 0 = log off, 1 = change password */
    for (;;) {
        paint_fill(C_BG);
        tui_row(0, "Makar " MAKAR_VERSION, C_SELFG, C_TITLE);
        tui_center(2, "System  (Ctrl-Alt-Del)", C_FG, C_BG);
        uint32_t r = s_rows / 2;
        tui_center(r - 1, sel == 0 ? "> Log off <" : "  Log off  ",
                   sel == 0 ? C_OK : C_FG, C_BG);
        tui_center(r + 1, sel == 1 ? "> Change password <" : "  Change password  ",
                   sel == 1 ? C_OK : C_FG, C_BG);
        tui_center(r + 3, "Up/Down + Enter   Esc cancels", C_DIM, C_BG);

        unsigned char c = keyboard_getchar();
        if (c == 27) break;                                      /* Esc cancel  */
        else if (c == (unsigned char)KEY_ARROW_UP)   sel = 0;
        else if (c == (unsigned char)KEY_ARROW_DOWN) sel = 1;
        else if (c == '\n' || c == '\r') {
            if (sel == 0) {                                      /* Log off     */
                vesa_tty_set_status_visible(prev_status);
                task_exit();                                     /* ends session */
            }
            passwd_screen(auth_current_user());                  /* Change pass */
            break;
        }
    }
    vesa_tty_set_status_visible(prev_status);
    vtty_request_repaint(VTTY_ROOT_SLOT);   /* restore the shell screen */
}

/* --------------------------------------------------------------------------
 * passwd_screen: full-screen TUI to change `user`'s password (login-like).
 * New + Confirm fields with Tab/Enter; writes /etc/shadow.  Caller gates this
 * to installed systems (writable rootfs).  Returns 0 on success, -1 otherwise.
 * --------------------------------------------------------------------------*/
int passwd_screen(const char *user)
{
    login_geometry();
    vesa_tty_set_status_visible(0);

    char p1[256], p2[256];
    uint32_t mid_col = s_cols / 2, mid_row = s_rows / 2;
    uint32_t box_w = 36;
    uint32_t box_left = (mid_col > box_w / 2) ? mid_col - box_w / 2 : 2;
    uint32_t label_col = box_left;
    uint32_t field_col = box_left + 12;
    uint32_t row_new = mid_row - 2, row_con = mid_row;
    uint32_t row_hint = mid_row + 2, row_err = mid_row + 4;

    for (;;) {
        paint_fill(C_BG);
        tui_row(0, "Makar " MAKAR_VERSION, C_SELFG, C_TITLE);
        tui_center(2, "Change Password", C_FG, C_BG);
        {
            char ub[80]; size_t p = 0;
            const char *pre = "User: ";
            while (*pre && p < sizeof(ub) - 1) ub[p++] = *pre++;
            for (const char *u = user; u && *u && p < sizeof(ub) - 1; u++) ub[p++] = *u;
            ub[p] = '\0';
            tui_center(4, ub, C_DIM, C_BG);
        }

        if (s_gui) {
            tui_at(label_col, row_new, "New:        ", C_FG, C_BG);
            tui_at(label_col, row_con, "Confirm:    ", C_FG, C_BG);
            tui_at(label_col, row_hint, "Tab switches fields, Enter saves", C_DIM, C_BG);
        } else {
            t_writestring("New password: ");
        }

        p1[0] = '\0'; p2[0] = '\0';
        size_t l1 = 0, l2 = 0; int field = 0;
        for (;;) {
            if (field == 0) {
                edit_field(p1, &l1, sizeof(p1), field_col, row_new, 1);
                field = 1;
            } else {
                int r = edit_field(p2, &l2, sizeof(p2), field_col, row_con, 1);
                if (r == 1) break;
                field = 0;
            }
        }

        if (strcmp(p1, p2) != 0) {
            if (s_gui) tui_at(label_col, row_err, "Passwords do not match.   ", C_WARN, C_BG);
            else       t_writestring("\npasswd: passwords do not match.\n");
            ksleep(100);
            continue;
        }

        int ok = (shadow_set_password(user, p1) == 0);
        if (s_gui)
            tui_at(label_col, row_err,
                   ok ? "Password updated.         " : "Update failed.            ",
                   ok ? C_OK : C_WARN, C_BG);
        else
            t_writestring(ok ? "\npasswd: updated.\n" : "\npasswd: failed.\n");
        if (s_gui) ksleep(80);
        vesa_tty_set_status_visible(1);
        return ok ? 0 : -1;
    }
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
