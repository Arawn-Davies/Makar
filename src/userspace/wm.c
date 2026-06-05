/*
 * wm.c -- gui.elf: minimal double-buffered window-manager PoC.
 *
 * Composites a desktop + one draggable "shell" window + a software mouse
 * cursor into an anon-mmap back buffer, then presents the whole frame via
 * SYS_FB_PRESENT.  Mouse via SYS_MOUSE_READ; keyboard via non-blocking stdin.
 *
 * PoC scope (see docs/plans/gui-wm.md + HANDOFF.md):
 *   - mouse cursor + drag the window by its title bar
 *   - close box for the terminal window; Exit GUI icon for the WM
 *   - the window body is a local text area that echoes typed keys (stand-in
 *     terminal; hosting the real sh.elf needs non-blocking pipe I/O -- deferred)
 *
 * Assumes a 32-bpp XRGB8888 framebuffer (QEMU Bochs VBE default): a back-buffer
 * pixel is 0x00RRGGBB and is memcpy'd verbatim to scanout by the kernel.
 */

#include "syscall.h"

/* Userspace has no <stdint.h> (-nostdinc); these are the only fixed-width
 * types wm.c needs.  unsigned int is 32-bit on i686. */
typedef unsigned int  uint32_t;
typedef unsigned char uint8_t;

#include "font8x8.h"            /* FONT8x8[128][8], MSB-left (unsigned char) */

/* ---- framebuffer / back buffer ----------------------------------------- */

static unsigned int  W, H;
static uint32_t     *bb;            /* back buffer, W*H, row-major */

#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define COL_DESK   RGB(0x1e,0x29,0x3b)
#define COL_WIN    RGB(0x1e,0x1e,0x1e)   /* xfce-terminal dark background     */
#define COL_TITLE  RGB(0x35,0x6a,0xa8)
#define COL_TITLE2 RGB(0x24,0x48,0x74)
#define COL_BORDER RGB(0x0a,0x0a,0x0a)
#define COL_TEXT   RGB(0xd3,0xd7,0xcf)   /* tango light grey (xterm-ish)      */
#define COL_PROMPT RGB(0x8a,0xe2,0x34)   /* tango green prompt                */
#define COL_CURSOR RGB(0xd3,0xd7,0xcf)   /* block cursor                      */
#define COL_CLOSE  RGB(0xc0,0x40,0x40)

static inline void px(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) return;
    bb[(unsigned)y * W + (unsigned)x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint32_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            px(xx, yy, c);
}

static void draw_char(int x, int y, unsigned char ch, uint32_t fg)
{
    if (ch >= 128) ch = '?';
    const uint8_t *g = FONT8x8[ch];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++)
            if (bits & (1u << col))      /* bit0 = leftmost, matches vesa_tty */
                px(x + col, y + row, fg);
    }
}

static void draw_str(int x, int y, const char *s, uint32_t fg)
{
    for (; *s; s++, x += 8) draw_char(x, y, (unsigned char)*s, fg);
}

/* ---- mouse cursor sprite (11x16): 'X' outline, '.' fill, ' ' transparent */

static const char *CURSOR[16] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X....XXXXXX",
    "X..X.X     ",
    "X.X X.X    ",
    "XX  X.X    ",
    "X    X.X   ",
    "      XX   ",
};

static void draw_cursor(int cx, int cy)
{
    for (int row = 0; row < 16; row++)
        for (int col = 0; CURSOR[row][col]; col++) {
            char p = CURSOR[row][col];
            if (p == 'X') px(cx + col, cy + row, 0x000000);
            else if (p == '.') px(cx + col, cy + row, 0xFFFFFF);
        }
}

/* ---- window + terminal stand-in ---------------------------------------- */

#define TH       18          /* title bar height */
#define PAD      4
#define TCOLS    64
#define TROWS    32

static int win_x = 220, win_y = 140, win_w = 480, win_h = 300;

static char term[TROWS][TCOLS];
static int  t_cols, t_rows, t_cr, t_cc;
static int  term_pid = -1, term_in = -1, term_out = -1;

static void term_clear(void)
{
    for (int r = 0; r < TROWS; r++)
        for (int c = 0; c < TCOLS; c++) term[r][c] = ' ';
    t_cr = t_cc = 0;
}

static void term_init(void)
{
    t_cols = (win_w - 2 * PAD) / 8; if (t_cols > TCOLS) t_cols = TCOLS;
    t_rows = (win_h - TH - 2 * PAD) / 8; if (t_rows > TROWS) t_rows = TROWS;
    term_clear();
}

static void term_newline(void)
{
    t_cc = 0;
    if (++t_cr >= t_rows) {                 /* scroll up one line */
        for (int r = 0; r < t_rows - 1; r++)
            for (int c = 0; c < TCOLS; c++) term[r][c] = term[r + 1][c];
        for (int c = 0; c < TCOLS; c++) term[t_rows - 1][c] = ' ';
        t_cr = t_rows - 1;
    }
}

static void term_putc(char ch)
{
    if (ch == '\n') { term_newline(); return; }
    if (ch == '\r') { t_cc = 0; return; }
    if (ch == 8 || ch == 127) {             /* backspace */
        if (t_cc > 0) { t_cc--; term[t_cr][t_cc] = ' '; }
        return;
    }
    if (ch < 32) return;
    if (t_cc >= t_cols) term_newline();
    term[t_cr][t_cc++] = ch;
}

static void term_write_key(int c)
{
    if (term_in < 0) return;
    unsigned char b = (unsigned char)c;
    if (b == '\r') b = '\n';
    (void)sys_write(term_in, &b, 1);
}

static void term_spawn_shell(void)
{
    if (term_pid > 0) return;
    int inpipe[2], outpipe[2];
    if (sys_pipe(inpipe) < 0 || sys_pipe(outpipe) < 0) {
        const char *m = "terminal: pipe failed\n";
        for (int i = 0; m[i]; i++) term_putc(m[i]);
        return;
    }

    int pid = sys_fork();
    if (pid < 0) {
        const char *m = "terminal: fork failed\n";
        for (int i = 0; m[i]; i++) term_putc(m[i]);
        return;
    }
    if (pid == 0) {
        sys_close(inpipe[1]);
        sys_close(outpipe[0]);
        sys_dup2(inpipe[0], 0);
        sys_dup2(outpipe[1], 1);
        sys_dup2(outpipe[1], 2);
        sys_close(inpipe[0]);
        sys_close(outpipe[1]);

        char user[48];
        char user_arg[64];
        char *av[4];
        av[0] = "sh.elf";
        av[1] = 0;
        av[2] = 0;
        av[3] = 0;
        if (sys_whoami(user, sizeof(user)) > 0) {
            const char *p = "--user=";
            int o = 0;
            for (int i = 0; p[i]; i++) user_arg[o++] = p[i];
            for (int i = 0; user[i] && o < (int)sizeof(user_arg) - 1; i++)
                user_arg[o++] = user[i];
            user_arg[o] = 0;
            av[1] = user_arg;
        }
        sys_execve("/apps/sh.elf", av, (char *const *)0);
        sys_exit(127);
    }

    term_pid = pid;
    term_in = inpipe[1];
    term_out = outpipe[0];
    sys_close(inpipe[0]);
    sys_close(outpipe[1]);
    sys_fcntl(term_out, F_SETFL, O_NONBLOCK);
    sys_fcntl(term_in, F_SETFL, O_NONBLOCK);
}

static void term_kill_shell(void)
{
    if (term_pid > 0) {
        sys_kill(term_pid, SIGKILL);
        int st = 0;
        sys_wait4(term_pid, &st, 0);     /* reap so it can't linger/hold state */
    }
    if (term_in  >= 0) sys_close(term_in);
    if (term_out >= 0) sys_close(term_out);
    term_pid = -1; term_in = -1; term_out = -1;
}

static int term_pump_output(void)
{
    if (term_out < 0) return 0;
    unsigned char buf[128];
    int dirty = 0;
    for (;;) {
        long n = sys_read(term_out, buf, sizeof(buf));
        if (n <= 0) break;
        for (long i = 0; i < n; i++) term_putc((char)buf[i]);
        dirty = 1;
        if (n < (long)sizeof(buf)) break;
    }
    return dirty;
}

static void draw_window(void)
{
    t_cols = (win_w - 2 * PAD) / 8; if (t_cols > TCOLS) t_cols = TCOLS;
    t_rows = (win_h - TH - 2 * PAD) / 8; if (t_rows > TROWS) t_rows = TROWS;

    /* border + body */
    fill_rect(win_x - 1, win_y - 1, win_w + 2, win_h + 2, COL_BORDER);
    fill_rect(win_x, win_y, win_w, win_h, COL_WIN);

    /* title bar (two-tone) + caption */
    fill_rect(win_x, win_y, win_w, TH, COL_TITLE);
    fill_rect(win_x, win_y + TH - 2, win_w, 2, COL_TITLE2);
    draw_str(win_x + PAD, win_y + 5, "Terminal  makar:~", 0xFFFFFF);

    /* close box */
    fill_rect(win_x + win_w - TH, win_y, TH, TH, COL_CLOSE);
    draw_str(win_x + win_w - TH + 5, win_y + 5, "x", 0xFFFFFF);

    /* terminal text */
    int bx = win_x + PAD, by = win_y + TH + PAD;
    for (int r = 0; r < t_rows; r++) {
        int prompt_row = (term[r][0] == 'm' && term[r][6] == '$');  /* makar:~$ */
        for (int c = 0; c < t_cols; c++)
            if (term[r][c] != ' ') {
                /* tint the "makar:~$" prompt green, like a shell prompt */
                uint32_t col = (prompt_row && c < 8) ? COL_PROMPT : COL_TEXT;
                draw_char(bx + c * 8, by + r * 8, (unsigned char)term[r][c], col);
            }
    }

    /* xterm-style block cursor at the input cell */
    if (t_cr < t_rows && t_cc < t_cols) {
        int cxp = bx + t_cc * 8, cyp = by + t_cr * 8;
        fill_rect(cxp, cyp, 8, 8, COL_CURSOR);
        if (term[t_cr][t_cc] != ' ')
            draw_char(cxp, cyp, (unsigned char)term[t_cr][t_cc], COL_WIN);
    }
}

static int in_titlebar(int x, int y)
{
    return x >= win_x && x < win_x + win_w - TH && y >= win_y && y < win_y + TH;
}
static int in_closebox(int x, int y)
{
    return x >= win_x + win_w - TH && x < win_x + win_w && y >= win_y && y < win_y + TH;
}

/* ---- input ------------------------------------------------------------- */

static int read_key_nb(void)
{
    unsigned char c;
    return (sys_read(0, &c, 1) == 1) ? (int)c : -1;
}

/* ---- bottom dock / taskbar with live resource stats -------------------- */

#define DOCK_H    32
#define COL_DOCK   RGB(0x12,0x16,0x1e)
#define COL_DOCK2  RGB(0x28,0x32,0x44)
#define COL_TILE   RGB(0x24,0x30,0x44)
#define COL_PILL   RGB(0x1b,0x22,0x2e)
#define COL_STAT   RGB(0xc8,0xd4,0xe2)
#define COL_ACCENT RGB(0x4c,0x8d,0xff)
#define COL_MEM    RGB(0x35,0xc7,0x59)
#define COL_TASK   RGB(0x4c,0x8d,0xff)
#define COL_UP     RGB(0xf0,0xa8,0x30)
#define COL_CLK    RGB(0x35,0xc7,0xcf)

static unsigned int stat_next = 0;
static unsigned int g_memu, g_memt, g_ntasks, g_up;
static char g_clock[12];
static char fbuf[2048];

static long rd_file(const char *path, char *buf, int sz)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, (unsigned)sz - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

/* Find `label` in buf, return the first run of digits after it (KiB). */
static unsigned int find_kb(const char *buf, const char *label)
{
    const char *p = buf;
    for (; *p; p++) {
        int i = 0;
        while (label[i] && p[i] == label[i]) i++;
        if (label[i] == 0) { p += i; break; }
    }
    while (*p && (*p < '0' || *p > '9')) p++;
    unsigned int v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10u + (unsigned)(*p++ - '0');
    return v;
}

static char *u2s(unsigned int v, char *out)
{
    char tmp[12]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10u); v /= 10u; }
    int j = 0; while (i) out[j++] = tmp[--i];
    out[j] = 0;
    return out + j;
}

static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

static void refresh_stats(void)
{
    if (rd_file("/proc/meminfo", fbuf, sizeof fbuf) > 0) {
        g_memu = find_kb(fbuf, "MemUsed")  / 1024u;
        g_memt = find_kb(fbuf, "MemTotal") / 1024u;
    }
    if (rd_file("/proc/tasks", fbuf, sizeof fbuf) > 0) {
        int n = 0; for (char *p = fbuf; *p; p++) if (*p == '\n') n++;
        if (n > 0) n--;                 /* drop the header row */
        g_ntasks = (unsigned)n;
    }
    g_up = sys_uptime() / 100u;
    if (rd_file("/proc/rtc", fbuf, sizeof fbuf) >= 19) {
        for (int i = 0; i < 8; i++) g_clock[i] = fbuf[11 + i];   /* HH:MM:SS */
        g_clock[8] = 0;
    } else g_clock[0] = 0;
}

/* Filled rect with the four corner pixels knocked out -> cheap rounded look. */
static void fill_round(int x, int y, int w, int h, uint32_t c)
{
    fill_rect(x, y, w, h, c);
    px(x, y, COL_DOCK);          px(x + w - 1, y, COL_DOCK);
    px(x, y + h - 1, COL_DOCK);  px(x + w - 1, y + h - 1, COL_DOCK);
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* A sketchybar-style stat pill: accent dot + text.  Returns width consumed. */
static int draw_pill(int x, int y, uint32_t accent, const char *text)
{
    int w = 14 + slen(text) * 8 + 8;
    fill_round(x, y, w, 18, COL_PILL);
    fill_round(x + 6, y + 6, 6, 6, accent);
    draw_str(x + 16, y + 5, text, COL_STAT);
    return w + 6;
}

static int draw_tile(int x, int y0, const char *label, int active)
{
    fill_round(x, y0 + 4, 30, DOCK_H - 8, COL_TILE);
    draw_str(x + (30 - slen(label) * 8) / 2, y0 + (DOCK_H - 8) / 2, label, 0xFFFFFF);
    if (active) fill_rect(x + 12, y0 + DOCK_H - 3, 6, 2, COL_ACCENT);  /* running dot */
    return 30 + 8;
}

static void draw_dock(void)
{
    int y0 = (int)H - DOCK_H;
    int my = y0 + (DOCK_H - 18) / 2;

    fill_rect(0, y0, (int)W, DOCK_H, COL_DOCK);
    fill_rect(0, y0, (int)W, 1, COL_DOCK2);          /* top hairline */

    /* left: Makar menu button + app tiles */
    int mw = 16 + 5 * 8 + 8;
    fill_round(8, my, mw, 18, COL_ACCENT);
    fill_round(8 + 6, my + 5, 8, 8, 0xFFFFFF);       /* logo swatch */
    draw_str(8 + 18, my + 5, "Makar", 0xFFFFFF);
    int x = 8 + mw + 10;
    x += draw_tile(x, y0, "sh", 1);
    x += draw_tile(x, y0, "+", 0);

    /* right: stat pills (mem, tasks, uptime, clock), right-aligned */
    char b[24]; char *d;
    char mem[24], tsk[16], upt[16];
    d = scat(mem, "MEM "); d = u2s(g_memu, d); d = scat(d, "/"); d = u2s(g_memt, d); scat(d, "M");
    d = scat(tsk, "TASKS "); u2s(g_ntasks, d);
    d = scat(upt, "UP "); d = u2s(g_up, d); scat(d, "s");
    (void)b;

    int wmem = 14 + slen(mem) * 8 + 8 + 6;
    int wtsk = 14 + slen(tsk) * 8 + 8 + 6;
    int wupt = 14 + slen(upt) * 8 + 8 + 6;
    int wclk = 14 + slen(g_clock) * 8 + 8 + 6;
    int rx = (int)W - 8 - (wmem + wtsk + wupt + wclk);
    rx += draw_pill(rx, my, COL_MEM,  mem);
    rx += draw_pill(rx, my, COL_TASK, tsk);
    rx += draw_pill(rx, my, COL_UP,   upt);
    if (g_clock[0]) draw_pill(rx, my, COL_CLK, g_clock);
}

/* ---- desktop icons ------------------------------------------------------ */

static int win_open    = 1;     /* terminal window visible?           */
static int saved_status = 1;    /* shell status-bar state before gui  */

#define ICON_COUNT 6
enum { ACT_TERM, ACT_FILES, ACT_EDITOR, ACT_DOOM, ACT_EXIT_GUI, ACT_LOGOUT };
typedef struct { int x, y, w, h; const char *label; int action; uint32_t tint; } icon_t;
static icon_t icons[ICON_COUNT] = {
    { 24,   40, 96, 70, "Terminal", ACT_TERM,     RGB(0x4c,0x8d,0xff) },
    { 136,  40, 96, 70, "Files",    ACT_FILES,    RGB(0xf0,0xa8,0x30) },
    { 24,  124, 96, 70, "Editor",   ACT_EDITOR,   RGB(0x35,0xc7,0x59) },
    { 136, 124, 96, 70, "Doom",     ACT_DOOM,     RGB(0xc0,0x40,0x40) },
    { 24,  208, 96, 70, "Exit GUI", ACT_EXIT_GUI, RGB(0x88,0x70,0xd8) },
    { 136, 208, 96, 70, "Logout",   ACT_LOGOUT,   RGB(0x80,0x88,0x98) },
};

static void draw_icons(void)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_t *ic = &icons[i];
        fill_round(ic->x, ic->y, ic->w, ic->h, RGB(0x2a,0x38,0x50));
        fill_round(ic->x + ic->w / 2 - 16, ic->y + 10, 32, 30, ic->tint);
        draw_str(ic->x + (ic->w - slen(ic->label) * 8) / 2, ic->y + ic->h - 16,
                 ic->label, 0xFFFFFF);
    }
}

static int icon_hit(int x, int y)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_t *ic = &icons[i];
        if (x >= ic->x && x < ic->x + ic->w && y >= ic->y && y < ic->y + ic->h)
            return ic->action;
    }
    return -1;
}

/* Launch a fullscreen app by replacing the WM image (execve hands kb+m focus
 * to the child; on its exit the launching shell regains focus).  Restores the
 * shell's terminal state first.  Returns only if execve fails. */
static void launch_fullscreen(const char *path)
{
    sys_fcntl(0, F_SETFL, 0);
    sys_statusbar_set(saved_status);
    sys_shell_clear();
    char *av[2]; av[0] = (char *)path; av[1] = 0;
    char *ev[1]; ev[0] = 0;
    sys_execve(path, av, ev);
    /* execve failed -- restore WM state and carry on */
    sys_fcntl(0, F_SETFL, O_NONBLOCK);
    sys_statusbar_set(0);
}

static void compose(int cx, int cy)
{
    fill_rect(0, 0, (int)W, (int)H, COL_DESK);
    draw_str(8, 8, "Makar desktop -- click an icon; drag the title bar",
             RGB(0x90,0xa0,0xb5));
    draw_icons();
    if (win_open) draw_window();
    draw_dock();                /* drawn every frame, on top -- never flickers */
    draw_cursor(cx, cy);
    sys_fb_present(bb);
}

/* ---- corner-click smoke test ('gui test') ------------------------------- */
/* Draws a 48x48 square in each screen corner; click each (turns green).
 * Emits "GUI-MOUSE-TEST: PASS" on serial when all four are hit. */
static int corner_test(void)
{
    const int S = 48;
    int sx[4] = { 0, (int)W - S, 0,         (int)W - S };
    int sy[4] = { 0, 0,         (int)H - S, (int)H - S };
    int hit[4] = { 0, 0, 0, 0 };

    int cx = (int)W / 2, cy = (int)H / 2, prev_left = 0, dirty = 1;
    for (;;) {
        unsigned int ev;
        while ((ev = sys_mouse_read()) != 0) {
            cx += (int)(signed char)((ev >> 8) & 0xFF);
            cy += (int)(signed char)((ev >> 16) & 0xFF);
            if (cx < 0) cx = 0;
            if (cx >= (int)W) cx = (int)W - 1;
            if (cy < 0) cy = 0;
            if (cy >= (int)H) cy = (int)H - 1;
            int left = ev & 1;
            if (left && !prev_left)
                for (int i = 0; i < 4; i++)
                    if (cx >= sx[i] && cx < sx[i] + S && cy >= sy[i] && cy < sy[i] + S)
                        hit[i] = 1;
            prev_left = left;
            dirty = 1;
        }
        while (read_key_nb() >= 0) {}

        if (dirty) {
            fill_rect(0, 0, (int)W, (int)H, COL_DESK);
            draw_str(8, (int)H / 2, "Click the square in each corner.",
                     RGB(0x90,0xa0,0xb5));
            int all = 1;
            for (int i = 0; i < 4; i++) {
                fill_rect(sx[i], sy[i], S, S, hit[i] ? RGB(0x30,0xb0,0x40)
                                                     : RGB(0xc0,0x40,0x40));
                if (!hit[i]) all = 0;
            }
            draw_cursor(cx, cy);
            sys_fb_present(bb);
            if (all) {
                const char *m = "GUI-MOUSE-TEST: PASS\n";
                int n = 0; while (m[n]) n++;
                sys_write_serial(m, n);
                goto done;
            }
            dirty = 0;
        }
        sys_yield();
    }
done:
    sys_shell_clear();
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    unsigned int info = sys_fb_info();
    if (!info) {
        const char *e = "gui: no pixel framebuffer (VGA-only)\n";
        sys_write(2, e, 36);
        return 1;
    }
    W = (info >> 16) & 0xFFFF;
    H = info & 0xFFFF;

    bb = (uint32_t *)sys_mmap(0, (unsigned long)W * H * 4,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bb == (uint32_t *)MAP_FAILED || !bb) {
        const char *e = "gui: back-buffer mmap failed\n";
        sys_write(2, e, 29);
        return 1;
    }

    sys_fcntl(0, F_SETFL, O_NONBLOCK);
    /* Force the shell status bar off while the WM owns the screen, but
     * remember its prior state so we restore the shell's choice on exit. */
    saved_status = sys_statusbar_enabled();
    sys_statusbar_set(0);
    /* Don't let Ctrl-C terminate the whole gui.  It is ordinary terminal
     * input for the hosted shell, not a WM close shortcut. */
    sys_signal(SIGINT, SIG_IGN);

    if (argc > 1 && argv[1][0] == 't') {       /* gui test */
        int rc = corner_test();
        sys_fcntl(0, F_SETFL, 0);
        sys_statusbar_set(saved_status);
        return rc;
    }

    term_init();
    term_spawn_shell();
    refresh_stats();
    stat_next = sys_uptime() + 100u;

    int cx = (int)W / 2, cy = (int)H / 2;
    int dragging = 0, drag_dx = 0, drag_dy = 0, prev_left = 0;
    int dirty = 1;

    for (;;) {
        /* refresh dock stats ~1 Hz; force a redraw so the clock ticks even
         * when the mouse is still */
        unsigned int now = sys_uptime();
        if ((int)(now - stat_next) >= 0) {
            refresh_stats();
            stat_next = now + 100u;
            dirty = 1;
        }
        if (term_pump_output()) dirty = 1;
        if (term_pid > 0) {
            int st = 0;
            int r = sys_wait4(term_pid, &st, WNOHANG);
            if (r == term_pid) {
                term_pid = -1;
                term_in = -1;
                term_out = -1;
                const char *m = "\n[terminal exited]\n";
                for (int i = 0; m[i]; i++) term_putc(m[i]);
                dirty = 1;
            }
        }

        /* drain mouse */
        unsigned int ev;
        while ((ev = sys_mouse_read()) != 0) {
            int dx = (int)(signed char)((ev >> 8) & 0xFF);
            int dy = (int)(signed char)((ev >> 16) & 0xFF);
            int left = (ev & 1);

            cx += dx; cy += dy;
            if (cx < 0) cx = 0;
            if (cx >= (int)W) cx = (int)W - 1;
            if (cy < 0) cy = 0;
            if (cy >= (int)H) cy = (int)H - 1;

            if (left && !prev_left) {                 /* press */
                int act = icon_hit(cx, cy);
                if (act == ACT_DOOM)        launch_fullscreen("/apps/doom.elf");
                else if (act == ACT_EDITOR) launch_fullscreen("/apps/vix.elf");
                else if (act == ACT_FILES)  launch_fullscreen("/apps/files.elf");
                else if (act == ACT_TERM) { win_open = 1; term_spawn_shell(); }
                else if (act == ACT_EXIT_GUI) { sys_gui_close(); goto done; }
                else if (act == ACT_LOGOUT) { sys_logout(); goto done; }
                else if (win_open && in_closebox(cx, cy)) {
                    win_open = 0; term_kill_shell();  /* close window, not the gui */
                } else if (win_open && in_titlebar(cx, cy)) {
                    dragging = 1; drag_dx = cx - win_x; drag_dy = cy - win_y;
                }
            } else if (!left && prev_left) {          /* release */
                dragging = 0;
            }
            if (dragging) {
                win_x = cx - drag_dx; win_y = cy - drag_dy;
                if (win_x < 0) win_x = 0;
                if (win_y < 0) win_y = 0;
                if (win_x + win_w > (int)W) win_x = (int)W - win_w;
                if (win_y + win_h > (int)H) win_y = (int)H - win_h;
            }
            prev_left = left;
            dirty = 1;
        }

        /* drain keyboard.  WWLD: Ctrl-C (0x03) is ordinary terminal input --
         * forward it to the shell (aborts the current line / interrupts a
         * running command); it does NOT close the window.  The window closes
         * via the [x] box or Exit GUI, which kill the hosted shell (SIGHUP-like). */
        int c;
        while ((c = read_key_nb()) >= 0) {
            if (win_open) term_write_key(c);
            dirty = 1;
        }

        if (dirty) { compose(cx, cy); dirty = 0; }
        sys_yield();
    }

done:
    term_kill_shell();                 /* don't orphan the hosted shell */
    sys_fcntl(0, F_SETFL, 0);
    sys_statusbar_set(saved_status);   /* restore the shell's status-bar choice */
    return 0;
}
