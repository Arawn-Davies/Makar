/*
 * clock.elf - centred date + time display driven by /proc/rtc.
 *
 * Reads /proc/rtc once per second (uptime-driven), parses the
 * "YYYY-MM-DD HH:MM:SS" line, and paints it centred in the active
 * pane via SYS_PUTCH_AT.  Same diff-render + non-blocking-stdin
 * pattern maktop uses, so the screen doesn't flicker and 'q' or
 * F10 exits cleanly.
 *
 * Quits and restores the shell palette on 'q' / F10.  Auto-clears
 * to the shell colours on exit so the prompt returns to a known
 * state regardless of how the operator left.
 */

#include "syscall.h"

#define MAX_COLS 160
#define MAX_ROWS 60

#define CLR_BG          VGA_CLR(VGA_LGREY,  VGA_BLACK)
#define CLR_TIME        VGA_CLR(VGA_LGREEN, VGA_BLACK)
#define CLR_DATE        VGA_CLR(VGA_LCYAN,  VGA_BLACK)
#define CLR_HINT        VGA_CLR(VGA_DGREY,  VGA_BLACK)

static int s_cols;
static int s_rows;

static tty_cell_t g_cells[MAX_COLS * MAX_ROWS];
static int        g_ncells;

typedef struct { unsigned char ch; unsigned char clr; } prev_cell_t;
static prev_cell_t s_prev[MAX_COLS * MAX_ROWS];

static char s_proc_rtc_buf[64];

static void invalidate_prev(void)
{
    for (int i = 0; i < (int)(sizeof(s_prev)/sizeof(s_prev[0])); i++) {
        s_prev[i].ch  = 0xFF;
        s_prev[i].clr = 0xFF;
    }
}

static inline void put(int col, int row, char c, unsigned char clr)
{
    if (col < 0 || row < 0 || col >= MAX_COLS || row >= MAX_ROWS) return;
    int key = row * MAX_COLS + col;
    if (s_prev[key].ch == (unsigned char)c && s_prev[key].clr == clr)
        return;
    s_prev[key].ch  = (unsigned char)c;
    s_prev[key].clr = clr;
    if (g_ncells < (int)(sizeof(g_cells)/sizeof(g_cells[0]))) {
        g_cells[g_ncells].col = (unsigned char)col;
        g_cells[g_ncells].row = (unsigned char)row;
        g_cells[g_ncells].ch  = (unsigned char)c;
        g_cells[g_ncells].clr = clr;
        g_ncells++;
    }
}

static void put_str(int col, int row, const char *s, unsigned char clr)
{
    while (*s) put(col++, row, *s++, clr);
}

static void put_pad(int col, int row, int width, unsigned char clr)
{
    for (int i = 0; i < width; i++) put(col + i, row, ' ', clr);
}

static void flush(void)
{
    if (g_ncells > 0) {
        sys_putch_at(g_cells, (unsigned int)g_ncells);
        g_ncells = 0;
    }
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static long read_file(const char *path, char *buf, unsigned int cap)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, cap - 1);
    sys_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* Parse "YYYY-MM-DD HH:MM:SS" into two output strings.  Returns 1 on
 * success, 0 on any parse failure (date/time left untouched). */
static int parse_rtc(const char *buf, char *date_out, char *time_out)
{
    /* The line is fixed-width except for the trailing newline: 10
     * chars date, space, 8 chars time = 19 chars.  Be tolerant of
     * variations (extra whitespace) by walking digit-by-digit. */
    unsigned int n = slen(buf);
    if (n < 19) return 0;
    for (int i = 0; i < 10; i++) {
        char c = buf[i];
        char want = ((i == 4) || (i == 7)) ? '-' : 0;
        if (want) { if (c != want) return 0; }
        else      { if (c < '0' || c > '9') return 0; }
        date_out[i] = c;
    }
    date_out[10] = '\0';
    if (buf[10] != ' ') return 0;
    for (int i = 0; i < 8; i++) {
        char c = buf[11 + i];
        char want = ((i == 2) || (i == 5)) ? ':' : 0;
        if (want) { if (c != want) return 0; }
        else      { if (c < '0' || c > '9') return 0; }
        time_out[i] = c;
    }
    time_out[8] = '\0';
    return 1;
}

static void draw_all(const char *date, const char *time_str)
{
    g_ncells = 0;
    for (int r = 0; r < s_rows; r++) put_pad(0, r, s_cols, CLR_BG);

    /* Centred date on (rows/2 - 1), centred time on (rows/2), hint
     * on (rows-1). */
    int cy = s_rows / 2;
    int dx = (s_cols - (int)slen(date)) / 2;
    int tx = (s_cols - (int)slen(time_str)) / 2;
    put_str(dx, cy - 1, date,     CLR_DATE);
    put_str(tx, cy,     time_str, CLR_TIME);

    const char *hint = "q / F10 to quit";
    int hx = (s_cols - (int)slen(hint)) / 2;
    put_str(hx, s_rows - 1, hint, CLR_HINT);

    flush();
    sys_set_cursor(0, s_rows - 1);
}

static int read_key_nb(void)
{
    unsigned char c;
    long r = sys_read(0, &c, 1);
    if (r == 1) return (int)c;
    return -1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    s_cols = (int)sys_term_cols();
    s_rows = (int)sys_term_rows();
    if (s_cols > MAX_COLS) s_cols = MAX_COLS;
    if (s_rows > MAX_ROWS) s_rows = MAX_ROWS;

    if (sys_fcntl(0, F_SETFL, O_NONBLOCK) != 0) {
        const char *e = "clock: sys_fcntl failed\n";
        unsigned int n = 0; while (e[n]) n++;
        sys_write_serial(e, n);
        return 1;
    }

    char date[16] = "YYYY-MM-DD";
    char time_str[16] = "HH:MM:SS";

    invalidate_prev();
    sys_tty_clear(CLR_BG);

    long n = read_file("/proc/rtc", s_proc_rtc_buf, sizeof(s_proc_rtc_buf));
    if (n > 0) parse_rtc(s_proc_rtc_buf, date, time_str);
    draw_all(date, time_str);

    unsigned int next = sys_uptime() + 100u;

    for (;;) {
        int c = read_key_nb();
        if (c >= 0) {
            if (c == 'q' || c == 'Q' || c == (int)KEY_F10) break;
            if (c == (int)KEY_FOCUS_GAIN) {
                invalidate_prev();
                draw_all(date, time_str);
            }
            continue;
        }
        unsigned int now = sys_uptime();
        if ((int)(now - next) >= 0) {
            long got = read_file("/proc/rtc", s_proc_rtc_buf,
                                 sizeof(s_proc_rtc_buf));
            if (got > 0) parse_rtc(s_proc_rtc_buf, date, time_str);
            draw_all(date, time_str);
            next = now + 100u;
        }
        sys_yield();
    }

    sys_fcntl(0, F_SETFL, 0);
    sys_shell_clear();
    return 0;
}
