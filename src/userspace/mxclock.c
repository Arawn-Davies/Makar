/*
 * mxclock.elf -- a desktop clock, as a makx client.  Reads /proc/rtc once a
 * second and draws a large digital time + date into its makx surface.  The GUI
 * peer of the fullscreen clock.elf; reuses the same /proc/rtc parse.
 */
#include "syscall.h"
#include "gui_gfx.h"
#include "makx.h"
#include "font8x8.h"

#define RGB GFX_RGB
#define COL_BG   RGB(0x10,0x14,0x1c)
#define COL_TIME RGB(0x8a,0xe2,0x34)
#define COL_DATE RGB(0xd3,0xd7,0xcf)
#define COL_DIM  RGB(0x60,0x6a,0x76)

static int slen(const char *s){ int n=0; while(s[n])n++; return n; }

static long read_file(const char *path, char *buf, unsigned cap)
{
    int fd = sys_open(path, O_RDONLY); if (fd < 0) return -1;
    long n = sys_read(fd, buf, cap - 1); sys_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* "YYYY-MM-DD HH:MM:SS" -> date[11], time[9].  1 ok / 0 parse fail. */
static int parse_rtc(const char *buf, char *date_out, char *time_out)
{
    if ((unsigned)slen(buf) < 19) return 0;
    for (int i = 0; i < 10; i++) {
        char c = buf[i], want = (i==4||i==7) ? '-' : 0;
        if (want ? (c != want) : (c < '0' || c > '9')) return 0;
        date_out[i] = c;
    }
    date_out[10] = '\0';
    if (buf[10] != ' ') return 0;
    for (int i = 0; i < 8; i++) {
        char c = buf[11+i], want = (i==2||i==5) ? ':' : 0;
        if (want ? (c != want) : (c < '0' || c > '9')) return 0;
        time_out[i] = c;
    }
    time_out[8] = '\0';
    return 1;
}

/* Draw one 8x8 glyph scaled by `sc` (each set pixel becomes an sc*sc block).
 * Matches gfx_char's bit order: bit 0 = leftmost column. */
static void glyph_scaled(gfx_surface *s, int x, int y, unsigned char ch, int sc, gfx_u32 fg)
{
    if (ch >= 128) ch = '?';
    const unsigned char *g = FONT8x8[ch];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (g[r] & (1u << c)) gfx_fill(s, x + c*sc, y + r*sc, sc, sc, fg);
}
static int str_scaled_w(const char *s, int sc){ return slen(s) * 8 * sc; }
static void str_scaled(gfx_surface *s, int x, int y, const char *str, int sc, gfx_u32 fg)
{
    for (; *str; str++, x += 8*sc) glyph_scaled(s, x, y, (unsigned char)*str, sc, fg);
}

int main(int argc, char **argv)
{
    mx_conn c;
    if (mx_connect(&c, argc, argv, 360, 200, MX_F_RESIZABLE) != 0) return 1;

    char date[16] = "----------", tstr[16] = "--:--:--";
    unsigned next = 0;          /* uptime tick of the next refresh */
    int first = 1;

    while (!c.closed) {
        mx_pump(&c);
        while (mx_key(&c) >= 0) { /* clock ignores keys */ }

        int tick_due = 0;
        unsigned now = sys_uptime();
        if (now >= next) {       /* USER_HZ = 100 -> +100 = 1 s */
            char buf[64];
            if (read_file("/proc/rtc", buf, sizeof buf) > 0)
                parse_rtc(buf, date, tstr);
            next = now + 100u;
            tick_due = 1;
        }

        if (tick_due || first || c.resized) {
            gfx_surface *s = &c.surf;
            gfx_fill(s, 0, 0, s->w, s->h, COL_BG);
            /* Pick a time scale that fits the width with margin. */
            int sc = (s->w - 16) / (8 * 8);          /* 8 glyphs "HH:MM:SS" */
            if (sc < 1) sc = 1;
            if (sc > 8) sc = 8;
            int tw = str_scaled_w(tstr, sc);
            int th = 8 * sc;
            int tx = (s->w - tw) / 2;
            int ty = (s->h - th) / 2 - 8;
            str_scaled(s, tx, ty, tstr, sc, COL_TIME);
            /* Date in plain 8x8, centred under the time. */
            int dx = (s->w - gfx_text_w(date)) / 2;
            gfx_str(s, dx, ty + th + 10, date, COL_DATE);
            mx_present(&c);
            first = 0;
        }
        sys_yield();
    }
    mx_close(&c);
    return 0;
}
