/*
 * maktop.elf - htop-style task viewer for Makar.
 *
 * Reads /proc/tasks + /proc/meminfo via sys_open / sys_read and draws a
 * full-screen grid via SYS_PUTCH_AT.  Modelled after htop: meter bars
 * (CPU + Mem) and summary text up top, columnar task list in the
 * middle, F-key bar at the bottom.  ↑/↓ select; F9 sends SIGTERM; s
 * prompts for an arbitrary signal number; F10 / q quits.
 *
 * Auto-refresh every 50 PIT ticks (~500 ms at 100 Hz).  POSIX-style
 * non-blocking stdin: sys_fcntl(0, F_SETFL, O_NONBLOCK), then sys_read
 * returns 1 byte or -EAGAIN.  Same pattern a real libc port (ELKS /
 * fuzix / musl) would use.
 */

#include "syscall.h"

/* Maktop is resolution-agnostic: the pane dimensions come from
 * sys_term_cols() / sys_term_rows() at startup.  The kernel pane API
 * returns the area excluding the tmux-style VT status bar, so we draw
 * the whole reported region and the status bar at row $rows stays
 * visible automatically.  Static buffers below are sized for a
 * comfortable maximum; we clip everything to the actual runtime
 * dimensions. */
#define MAX_COLS        160
#define MAX_ROWS        60

static int s_cols;
static int s_rows;

/* Layout derived from s_rows at startup (set in main). */
static int HDR_ROW_CPU;
static int HDR_ROW_MEM;
static int ROW_COL_HDR;
static int ROW_LIST_FIRST;
static int ROW_LIST_LAST;
static int ROW_STATUS;
static int ROW_FOOTER;
static int MAX_TASKS_VIEW;

#define NAME_W          16
#define MAX_TASKS_CAP   32

/* Palette - approximates htop's defaults. */
#define CLR_BG          VGA_CLR(VGA_LGREY,  VGA_BLACK)
#define CLR_HDR_LABEL   VGA_CLR(VGA_LCYAN,  VGA_BLACK)
#define CLR_HDR_TEXT    VGA_CLR(VGA_WHITE,  VGA_BLACK)
#define CLR_BAR_BRACKET VGA_CLR(VGA_LGREY,  VGA_BLACK)
#define CLR_BAR_FILL    VGA_CLR(VGA_LGREEN, VGA_BLACK)
#define CLR_BAR_FILL_HI VGA_CLR(VGA_YELLOW, VGA_BLACK)
#define CLR_BAR_FILL_CR VGA_CLR(VGA_LRED,   VGA_BLACK)
#define CLR_BAR_EMPTY   VGA_CLR(VGA_DGREY,  VGA_BLACK)
#define CLR_COLHDR      VGA_CLR(VGA_BLACK,  VGA_LGREEN)
#define CLR_ROW         VGA_CLR(VGA_LGREY,  VGA_BLACK)
#define CLR_ROW_SEL     VGA_CLR(VGA_BLACK,  VGA_LCYAN)
#define CLR_STATUS      VGA_CLR(VGA_YELLOW, VGA_BLACK)
#define CLR_STATUS_ERR  VGA_CLR(VGA_LRED,   VGA_BLACK)
#define CLR_FKEY_NUM    VGA_CLR(VGA_WHITE,  VGA_BLACK)
#define CLR_FKEY_LABEL  VGA_CLR(VGA_BLACK,  VGA_LCYAN)

typedef struct {
    int          pid;
    char         name[NAME_W + 1];
    char         state[8];
    int          tty;
    unsigned int kticks;
    unsigned int kticks_prev;
} task_row_t;

static task_row_t s_task_rows[MAX_TASKS_CAP];
static int        s_nrows;
static int        s_sel;

static unsigned int s_total_kticks_prev;
static unsigned int s_uptime_prev;
static unsigned int s_cpu_pct;

static tty_cell_t g_cells[MAX_COLS * MAX_ROWS];
static int        g_ncells;

/* Diff buffer.  We keep the last (ch, clr) we sent for every cell;
 * put() skips re-queueing cells that haven't changed.  Eliminates the
 * flicker that came from repainting 1920 cells per refresh on the
 * VESA framebuffer (each glyph blit is visible).  ~19 KiB at MAX
 * dimensions, lives in .bss. */
typedef struct { unsigned char ch; unsigned char clr; } prev_cell_t;
static prev_cell_t s_prev[MAX_COLS * MAX_ROWS];

/* Force the next put() pass to emit every cell (after switching modes
 * like picker overlay → restore, or on first frame). */
static void invalidate_prev(void)
{
    for (int i = 0; i < (int)(sizeof(s_prev)/sizeof(s_prev[0])); i++) {
        s_prev[i].ch  = 0xFF;
        s_prev[i].clr = 0xFF;
    }
}

/* ---- draw helpers ---- */

static inline void put(int col, int row, char c, unsigned char clr)
{
    if (col < 0 || row < 0 || col >= MAX_COLS || row >= MAX_ROWS) return;
    int key = row * MAX_COLS + col;
    if (s_prev[key].ch == (unsigned char)c && s_prev[key].clr == clr)
        return;
    s_prev[key].ch  = (unsigned char)c;
    s_prev[key].clr = clr;
    if (g_ncells < (int)(sizeof(g_cells) / sizeof(g_cells[0]))) {
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

/* ---- string utilities ---- */

static unsigned int slen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void uitoa(unsigned int v, char *buf)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static int parse_uint(const char **pp, unsigned int *out)
{
    const char *p = *pp;
    unsigned int v = 0;
    int got = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (unsigned int)(*p - '0');
        p++; got = 1;
    }
    *pp = p;
    *out = v;
    return got;
}

/* ---- /proc readers ---- */

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

static int parse_tasks_line(const char *line, task_row_t *r)
{
    const char *p = line;
    unsigned int v;

    if (!parse_uint(&p, &v)) return 0;
    r->pid = (int)v;
    while (*p == ' ') p++;

    int i = 0;
    while (*p && *p != ' ' && i < NAME_W) r->name[i++] = *p++;
    r->name[i] = '\0';
    while (*p == ' ') p++;

    i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(r->state) - 1)
        r->state[i++] = *p++;
    r->state[i] = '\0';
    while (*p == ' ') p++;

    if (*p == '-') { r->tty = -1; p++; }
    else if (parse_uint(&p, &v)) r->tty = (int)v;
    else return 0;
    while (*p == ' ') p++;

    if (!parse_uint(&p, &v)) return 0;
    r->kticks = v;
    return 1;
}

/* Static read buffers: the user stack is one 4 KiB page (see
 * USER_STACK_TOP in usertest.c) and locals like a 4 KiB char[] would
 * straddle the bottom guard and page-fault.  None of these helpers
 * recurse or run concurrently, so .bss is the natural home. */
static char s_proc_tasks_buf[4096];
static char s_proc_meminfo_buf[1024];

static void refresh_tasks(void)
{
    long n = read_file("/proc/tasks", s_proc_tasks_buf, sizeof(s_proc_tasks_buf));
    if (n <= 0) { s_nrows = 0; return; }

    /* Move prev[] off the stack -- the user task gets one 4 KiB stack
     * page, and a VLA of MAX_TASKS_CAP entries (~1.5 KiB) plus the
     * other locals here was close enough to overflow it on the first
     * iteration. */
    static task_row_t prev[MAX_TASKS_CAP];
    int prev_n = s_nrows;
    for (int i = 0; i < prev_n; i++) prev[i] = s_task_rows[i];

    s_nrows = 0;
    char *line = s_proc_tasks_buf;
    while (*line && s_nrows < MAX_TASKS_VIEW) {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';
        task_row_t r;
        if (parse_tasks_line(line, &r)) {
            r.kticks_prev = 0;
            for (int i = 0; i < prev_n; i++) {
                if (prev[i].pid == r.pid) {
                    r.kticks_prev = prev[i].kticks;
                    break;
                }
            }
            s_task_rows[s_nrows++] = r;
        }
        if (!saved) break;
        line = eol + 1;
    }

    if (s_sel >= s_nrows) s_sel = s_nrows - 1;
    if (s_sel < 0) s_sel = 0;

    /* CPU% over the snapshot interval.  Exclude pid=1 (idle): at every
     * PIT IRQ some task is current and gets credited, and when nothing
     * else is runnable that's always idle -- so a naive sum would always
     * read 100% utilisation.  Linux excludes idle the same way (the
     * "idle" column in /proc/stat is the "subtract this for CPU%"
     * bucket). */
    unsigned int total = 0;
    for (int i = 0; i < s_nrows; i++) {
        if (s_task_rows[i].pid == 1) continue;
        total += s_task_rows[i].kticks;
    }
    unsigned int now = sys_uptime();
    unsigned int dt_real  = (now > s_uptime_prev) ? (now - s_uptime_prev) : 1u;
    unsigned int dt_total = (total > s_total_kticks_prev)
                            ? (total - s_total_kticks_prev) : 0u;
    s_cpu_pct = (dt_real == 0) ? 0u : (dt_total * 100u) / dt_real;
    if (s_cpu_pct > 100u) s_cpu_pct = 100u;
    s_total_kticks_prev = total;
    s_uptime_prev       = now;
}

static unsigned int meminfo_field(const char *buf, const char *label)
{
    unsigned int ll = slen(label);
    const char *p = buf;
    while (*p) {
        const char *ls = p;
        const char *eol = p;
        while (*eol && *eol != '\n') eol++;
        if ((unsigned int)(eol - ls) > ll && ls[0] == label[0]) {
            int match = 1;
            for (unsigned int i = 0; i < ll; i++) {
                if (ls[i] != label[i]) { match = 0; break; }
            }
            if (match) {
                const char *q = ls + ll;
                while (*q && (*q == ':' || *q == ' ' || *q == '\t')) q++;
                unsigned int v = 0;
                if (parse_uint(&q, &v)) return v;
            }
        }
        if (!*eol) break;
        p = eol + 1;
    }
    return 0;
}

/* ---- drawing ---- */

static void draw_bar(int col, int row, int inner_w, unsigned int pct,
                     const char *value_str)
{
    if (pct > 100u) pct = 100u;
    int filled = (int)((pct * (unsigned int)inner_w) / 100u);
    unsigned char fill_clr =
        (pct >= 90u) ? CLR_BAR_FILL_CR :
        (pct >= 50u) ? CLR_BAR_FILL_HI : CLR_BAR_FILL;

    put(col, row, '[', CLR_BAR_BRACKET);
    for (int i = 0; i < inner_w; i++) {
        int c = col + 1 + i;
        if (i < filled) put(c, row, '|', fill_clr);
        else            put(c, row, ' ', CLR_BAR_EMPTY);
    }
    put(col + 1 + inner_w, row, ']', CLR_BAR_BRACKET);

    unsigned int vl = slen(value_str);
    int vstart = col + 1 + inner_w - (int)vl;
    if (vstart < col + 1) vstart = col + 1;
    put_str(vstart, row, value_str, CLR_HDR_TEXT);
}

static void draw_header(void)
{
    char tmp[32];

    /* CPU row. */
    put_str(0, HDR_ROW_CPU, "CPU", CLR_HDR_LABEL);
    char pctbuf[8];
    uitoa(s_cpu_pct, pctbuf);
    {
        unsigned int n = slen(pctbuf);
        pctbuf[n] = '%'; pctbuf[n+1] = '\0';
    }
    draw_bar(4, HDR_ROW_CPU, 40, s_cpu_pct, pctbuf);

    put_str(48, HDR_ROW_CPU, "Tasks: ", CLR_HDR_LABEL);
    uitoa((unsigned int)s_nrows, tmp);
    put_str(55, HDR_ROW_CPU, tmp, CLR_HDR_TEXT);

    /* Mem row. */
    unsigned int total_kb = 0, free_kb = 0;
    long n = read_file("/proc/meminfo", s_proc_meminfo_buf, sizeof(s_proc_meminfo_buf));
    if (n > 0) {
        total_kb = meminfo_field(s_proc_meminfo_buf, "MemTotal");
        free_kb  = meminfo_field(s_proc_meminfo_buf, "MemFree");
    }
    unsigned int used_kb = (total_kb > free_kb) ? (total_kb - free_kb) : 0u;
    unsigned int mem_pct = (total_kb == 0u) ? 0u
                            : (used_kb * 100u) / total_kb;

    put_str(0, HDR_ROW_MEM, "Mem", CLR_HDR_LABEL);
    char memval[24];
    {
        char a[12], b[12];
        uitoa(used_kb / 1024u, a);
        uitoa(total_kb / 1024u, b);
        int o = 0;
        for (unsigned int i = 0; a[i]; i++) memval[o++] = a[i];
        memval[o++] = 'M'; memval[o++] = '/';
        for (unsigned int i = 0; b[i]; i++) memval[o++] = b[i];
        memval[o++] = 'M'; memval[o] = '\0';
    }
    draw_bar(4, HDR_ROW_MEM, 40, mem_pct, memval);

    put_str(48, HDR_ROW_MEM, "Uptime: ", CLR_HDR_LABEL);
    unsigned int ticks = sys_uptime();
    unsigned int secs  = ticks / 100u;
    unsigned int hh = secs / 3600u, mm = (secs % 3600u) / 60u, ss = secs % 60u;
    int col = 56;
    uitoa(hh, tmp); put_str(col, HDR_ROW_MEM, tmp, CLR_HDR_TEXT); col += (int)slen(tmp);
    put(col++, HDR_ROW_MEM, 'h', CLR_HDR_TEXT);
    put(col++, HDR_ROW_MEM, ' ', CLR_BG);
    uitoa(mm, tmp); put_str(col, HDR_ROW_MEM, tmp, CLR_HDR_TEXT); col += (int)slen(tmp);
    put(col++, HDR_ROW_MEM, 'm', CLR_HDR_TEXT);
    put(col++, HDR_ROW_MEM, ' ', CLR_BG);
    uitoa(ss, tmp); put_str(col, HDR_ROW_MEM, tmp, CLR_HDR_TEXT); col += (int)slen(tmp);
    put(col++, HDR_ROW_MEM, 's', CLR_HDR_TEXT);
}

static void draw_column_headers(void)
{
    put_pad(0, ROW_COL_HDR, s_cols, CLR_COLHDR);
    put_str(1,  ROW_COL_HDR, "PID",   CLR_COLHDR);
    put_str(6,  ROW_COL_HDR, "NAME",  CLR_COLHDR);
    put_str(24, ROW_COL_HDR, "STATE", CLR_COLHDR);
    put_str(32, ROW_COL_HDR, "TTY",   CLR_COLHDR);
    put_str(38, ROW_COL_HDR, "TICKS", CLR_COLHDR);
    put_str(48, ROW_COL_HDR, "DELTA", CLR_COLHDR);
}

static void draw_rows(void)
{
    char tmp[16];
    for (int i = 0; i < MAX_TASKS_VIEW; i++) {
        int row = ROW_LIST_FIRST + i;
        unsigned char clr = (i == s_sel) ? CLR_ROW_SEL : CLR_ROW;
        put_pad(0, row, s_cols, clr);

        if (i >= s_nrows) continue;
        task_row_t *r = &s_task_rows[i];

        uitoa((unsigned int)r->pid, tmp);
        put_str(1, row, tmp, clr);
        put_str(6, row, r->name, clr);
        put_str(24, row, r->state, clr);
        if (r->tty < 0) put(32, row, '-', clr);
        else { uitoa((unsigned int)r->tty, tmp); put_str(32, row, tmp, clr); }
        uitoa(r->kticks, tmp);
        put_str(38, row, tmp, clr);

        unsigned int delta = (r->kticks >= r->kticks_prev)
                             ? (r->kticks - r->kticks_prev) : 0u;
        uitoa(delta, tmp);
        put_str(48, row, tmp, clr);
    }
}

/* htop-style footer: "F1Help F2Setup ..." with numbers in white-on-black
 * and labels in black-on-cyan.  Only F9 / F10 are wired; the others are
 * placeholders so the visual matches htop. */
static void draw_footer(void)
{
    static const struct { const char *num; const char *lbl; } keys[] = {
        { "F1",  "Help  "  },
        { "F2",  "Setup "  },
        { "F3",  "Search"  },
        { "F5",  "Tree  "  },
        { "F6",  "SortBy"  },
        { "F9",  "Kill  "  },
        { "F10", "Quit  "  },
    };
    int col = 0;
    put_pad(0, ROW_FOOTER, s_cols, CLR_FKEY_LABEL);
    for (unsigned int i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        const char *n = keys[i].num;
        const char *l = keys[i].lbl;
        for (; *n && col < s_cols; n++) put(col++, ROW_FOOTER, *n, CLR_FKEY_NUM);
        for (; *l && col < s_cols; l++) put(col++, ROW_FOOTER, *l, CLR_FKEY_LABEL);
    }
}

static void draw_all(void)
{
    g_ncells = 0;
    for (int r = 0; r < s_rows; r++) put_pad(0, r, s_cols, CLR_BG);
    draw_header();
    draw_column_headers();
    draw_rows();
    draw_footer();
    flush();
    sys_set_cursor(0, ROW_STATUS);
}

/* ---- key reading ---- */

static int read_key_nb(void)
{
    unsigned char c;
    long r = sys_read(0, &c, 1);
    if (r == 1) return (int)c;
    return -1;
}

static int wait_key(void)
{
    for (;;) {
        int c = read_key_nb();
        if (c >= 0) return c;
        sys_yield();
    }
}

/* ---- status / prompt ---- */

static void show_status(const char *msg, unsigned char clr)
{
    g_ncells = 0;
    put_pad(0, ROW_STATUS, s_cols, clr);
    put_str(1, ROW_STATUS, msg, clr);
    flush();
}

static int prompt_signo(int pid)
{
    g_ncells = 0;
    put_pad(0, ROW_STATUS, s_cols, CLR_STATUS);
    put_str(1, ROW_STATUS, "Send signal (1..31) to PID ", CLR_STATUS);
    char pbuf[12]; uitoa((unsigned int)pid, pbuf);
    put_str(28, ROW_STATUS, pbuf, CLR_STATUS);
    put(28 + (int)slen(pbuf), ROW_STATUS, ':', CLR_STATUS);
    int input_col = 28 + (int)slen(pbuf) + 2;
    flush();

    char buf[8];
    int n = 0;
    for (;;) {
        int c = wait_key();
        if (c == '\n' || c == '\r') break;
        if (c == 0x1B) return -1;
        if (c == 8 || c == 0x7F) {
            if (n > 0) {
                n--;
                g_ncells = 0;
                put(input_col + n, ROW_STATUS, ' ', CLR_STATUS);
                flush();
            }
            continue;
        }
        if (c >= '0' && c <= '9' && n < (int)sizeof(buf) - 1) {
            buf[n] = (char)c;
            g_ncells = 0;
            put(input_col + n, ROW_STATUS, (char)c, CLR_STATUS);
            flush();
            n++;
        }
    }
    if (n == 0) return -1;
    buf[n] = '\0';
    const char *p = buf;
    unsigned int v;
    if (!parse_uint(&p, &v)) return -1;
    if (v < 1 || v > 31) return -1;
    return (int)v;
}

/* ---- htop-style left-panel signal picker ----
 *
 * F9 (or k) overlays a scrollable list of named signals on the left
 * side of the screen.  Up/Down navigate, Enter sends to the highlighted
 * task in the underlying list, Esc cancels.  Returns the chosen signo
 * (1..31) or -1 on cancel. */

static const struct { int signo; const char *name; } SIGNALS[] = {
    {  1, "SIGHUP"   },
    {  2, "SIGINT"   },
    {  3, "SIGQUIT"  },
    {  4, "SIGILL"   },
    {  5, "SIGTRAP"  },
    {  6, "SIGABRT"  },
    {  8, "SIGFPE"   },
    {  9, "SIGKILL"  },
    { 10, "SIGUSR1"  },
    { 11, "SIGSEGV"  },
    { 12, "SIGUSR2"  },
    { 13, "SIGPIPE"  },
    { 14, "SIGALRM"  },
    { 15, "SIGTERM"  },
    { 17, "SIGCHLD"  },
    { 18, "SIGCONT"  },
    { 19, "SIGSTOP"  },
    { 20, "SIGTSTP"  },
};

#define PICKER_W 20

#define CLR_PICK_HDR  VGA_CLR(VGA_BLACK,  VGA_LGREEN)
#define CLR_PICK_ROW  VGA_CLR(VGA_LGREY,  VGA_BLACK)
#define CLR_PICK_SEL  VGA_CLR(VGA_BLACK,  VGA_LCYAN)
#define CLR_PICK_HINT VGA_CLR(VGA_LCYAN,  VGA_BLACK)

static void draw_picker(int sel, int target_pid)
{
    char tmp[16];
    int total = (int)(sizeof(SIGNALS)/sizeof(SIGNALS[0]));

    /* Header row across the picker width. */
    put_pad(0, 0, PICKER_W, CLR_PICK_HDR);
    put_str(1, 0, "Send signal:", CLR_PICK_HDR);
    /* Show target PID compactly on the same header line if there's room. */
    {
        uitoa((unsigned int)target_pid, tmp);
        unsigned int n = slen(tmp);
        int col = PICKER_W - (int)n - 1;
        if (col > 13) put_str(col, 0, tmp, CLR_PICK_HDR);
    }

    /* Body rows: scroll so sel is always visible.  ROW_FOOTER and
     * one above it are reserved for the hint line.  We don't draw on
     * the kernel's status row (s_rows is the pane height; kernel
     * status bar lives at row s_rows). */
    int body_top    = 1;
    int body_bottom = s_rows - 2;       /* leave one row for hint */
    int body_h      = body_bottom - body_top + 1;
    int top = sel - body_h / 2;
    if (top < 0) top = 0;
    if (top > total - body_h) top = total - body_h;
    if (top < 0) top = 0;

    for (int row = body_top; row <= body_bottom; row++) {
        int idx = top + (row - body_top);
        unsigned char clr = (idx == sel) ? CLR_PICK_SEL : CLR_PICK_ROW;
        put_pad(0, row, PICKER_W, clr);
        if (idx < 0 || idx >= total) continue;
        uitoa((unsigned int)SIGNALS[idx].signo, tmp);
        /* Right-align signo in cols 1..3. */
        int slot = 3 - (int)slen(tmp);
        if (slot < 1) slot = 1;
        put_str(slot, row, tmp, clr);
        put_str(5, row, SIGNALS[idx].name, clr);
    }

    /* Hint row at the bottom of the picker. */
    put_pad(0, s_rows - 1, PICKER_W, CLR_PICK_HINT);
    put_str(1, s_rows - 1, " up/dn  ent send ", CLR_PICK_HINT);

    flush();
}

static int signal_picker(int target_pid)
{
    int sel = 13;   /* index of SIGTERM in the table -- safe default */
    int total = (int)(sizeof(SIGNALS)/sizeof(SIGNALS[0]));

    invalidate_prev();    /* picker overlays the underlying view */
    draw_picker(sel, target_pid);

    for (;;) {
        int c = wait_key();
        if (c == 0x1B || c == 'q' || c == 'Q') return -1;     /* Esc / q */
        if (c == '\n' || c == '\r') return SIGNALS[sel].signo;
        if (c == KEY_ARROW_UP   && sel > 0)            { sel--; draw_picker(sel, target_pid); }
        else if (c == KEY_ARROW_DOWN && sel < total-1) { sel++; draw_picker(sel, target_pid); }
    }
}

/* htop's default refresh is 1.5 s; we use 1 s here.  Faster than that
 * the digit cells flicker visibly on the VESA framebuffer. */
#define REFRESH_TICKS 100u

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* Pull the live pane dimensions.  These already exclude the VT
     * status bar -- the kernel renders that on the row immediately
     * below our pane, so we can use the whole reported area. */
    s_cols = (int)sys_term_cols();
    s_rows = (int)sys_term_rows();
    if (s_cols > MAX_COLS) s_cols = MAX_COLS;
    if (s_rows > MAX_ROWS) s_rows = MAX_ROWS;
    if (s_cols < 70 || s_rows < 10) {
        const char *e = "maktop: needs >= 70 cols and 10 rows of screen\n";
        unsigned int n = 0; while (e[n]) n++;
        sys_write_serial(e, n);
        return 2;
    }

    /* Layout (top-down): two header rows, a blank separator, the
     * column header bar, then the task list, then status + footer. */
    HDR_ROW_CPU    = 0;
    HDR_ROW_MEM    = 1;
    ROW_COL_HDR    = 3;
    ROW_LIST_FIRST = 4;
    ROW_FOOTER     = s_rows - 1;
    ROW_STATUS     = s_rows - 2;
    ROW_LIST_LAST  = s_rows - 3;
    MAX_TASKS_VIEW = ROW_LIST_LAST - ROW_LIST_FIRST + 1;
    if (MAX_TASKS_VIEW > MAX_TASKS_CAP) MAX_TASKS_VIEW = MAX_TASKS_CAP;

    if (sys_fcntl(0, F_SETFL, O_NONBLOCK) != 0) {
        const char *e = "maktop: sys_fcntl failed\n";
        unsigned int n = 0; while (e[n]) n++;
        sys_write_serial(e, n);
        return 1;
    }

    s_uptime_prev       = sys_uptime();
    s_total_kticks_prev = 0;
    unsigned int next_refresh = s_uptime_prev + REFRESH_TICKS;

    /* First frame: force every cell out so the diff buffer reflects
     * what's actually on the framebuffer.  Without this the put()
     * skip-if-unchanged would suppress cells whose new (ch, clr)
     * matched the .bss-zeroed prev buffer (0x00 black-on-black). */
    invalidate_prev();
    sys_tty_clear(CLR_BG);
    refresh_tasks();
    draw_all();

    for (;;) {
        int c = read_key_nb();

        if (c >= 0) {
            if (c == 'q' || c == 'Q' || c == (int)KEY_F10) break;

            if (c == KEY_ARROW_UP) {
                if (s_sel > 0) s_sel--;
                draw_all();
            } else if (c == KEY_ARROW_DOWN) {
                if (s_sel < s_nrows - 1) s_sel++;
                draw_all();
            } else if (c == (int)KEY_F9 || c == 'k' || c == 'K') {
                if (s_nrows > 0) {
                    int pid = s_task_rows[s_sel].pid;
                    int sig = signal_picker(pid);
                    /* Picker overlaid the left column; force a full
                     * repaint of the underlying view to wipe it. */
                    invalidate_prev();
                    draw_all();
                    if (sig > 0) {
                        int rc = sys_kill(pid, sig);
                        show_status((rc == 0) ? " signal sent "
                                              : " sys_kill failed ",
                                    (rc == 0) ? CLR_STATUS : CLR_STATUS_ERR);
                    }
                }
            } else if (c == 's' || c == 'S') {
                /* Numeric-entry alternate path -- handy for muscle
                 * memory.  Same effect as the picker but typed. */
                if (s_nrows > 0) {
                    int pid = s_task_rows[s_sel].pid;
                    int sig = prompt_signo(pid);
                    if (sig > 0) {
                        int rc = sys_kill(pid, sig);
                        show_status((rc == 0) ? " signal sent "
                                              : " sys_kill failed ",
                                    (rc == 0) ? CLR_STATUS : CLR_STATUS_ERR);
                    } else {
                        draw_all();
                    }
                }
            } else if (c == 'r' || c == 'R') {
                refresh_tasks();
                draw_all();
                next_refresh = sys_uptime() + REFRESH_TICKS;
            } else if (c == (int)KEY_FOCUS_GAIN) {
                /* Returning to our VT after the user Alt+Fn'd away.
                 * The framebuffer is currently showing whichever VT
                 * they switched through, not our last frame; the
                 * diff buffer would only repaint cells that differ
                 * from our internal s_prev (the stale frame).
                 * Invalidate so the next draw_all is unconditional
                 * and we own the screen again. */
                invalidate_prev();
                refresh_tasks();
                draw_all();
                next_refresh = sys_uptime() + REFRESH_TICKS;
            }
            continue;
        }

        unsigned int now = sys_uptime();
        if ((int)(now - next_refresh) >= 0) {
            refresh_tasks();
            draw_all();
            next_refresh = now + REFRESH_TICKS;
        }
        sys_yield();
    }

    sys_fcntl(0, F_SETFL, 0);
    /* Restore the shell's default palette (same code path as the
     * `clear` shell builtin and what vix uses on exit).  Our
     * sys_tty_clear with our own bg colour leaves the screen black,
     * which doesn't match the operator's expectation of returning to
     * the shell colours. */
    sys_shell_clear();
    return 0;
}
