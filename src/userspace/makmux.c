/*
 * makmux.c -- userspace VT multiplexer scaffold.
 *
 * mak.sh0 starts this explicitly with `makmux`.  The mux process stays
 * alive on the root tty and owns the child login shells mak.sh1..mak.sh4.
 * Each child attaches itself to one of the four kernel VT slots, then execs
 * the normal userspace shell.  Keep this freestanding: in-OS TCC must be
 * able to rebuild it from /src/userspace/makmux.c.
 */

#include "syscall.h"

#define CHILD_COUNT 4
#define STATUS_LABEL_COL 1
#define STATUS_LABEL_W   18
#define STATUS_BAR_CLR   ((VGA_BLACK << 4) | VGA_LGREY)
#define STATUS_ACTIVE_CLR ((VGA_YELLOW << 4) | VGA_BLACK)

static int s_child_pids[CHILD_COUNT];
static int s_child_focus = 0;
static int s_clock_mode = 0;
static unsigned int s_last_state = 0xFFFFFFFFu;
static unsigned int s_last_tick = 0xFFFFFFFFu;

static void put_s(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

static void put_dec(int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) {
        char m = '-';
        sys_write(1, &m, 1);
        v = -v;
    }
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char c = tmp[--n];
        sys_write(1, &c, 1);
    }
}

static void child_shell(void)
{
    int slot = sys_vt_enter(s_child_focus);
    if (slot < 0) {
        put_s("makmux: no VT slot available for child shell\n");
        sys_exit(1);
    }

    char *argv[] = { "sh.elf", "--makmux", 0 };
    sys_execve("/apps/sh.elf", argv, (char *const *)0);
    put_s("makmux: exec /apps/sh.elf failed on VT ");
    put_dec(slot);
    put_s("\n");
    sys_exit(127);
}

static int spawn_child(int idx, int focus)
{
    s_child_focus = focus;
    int pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) child_shell();
    s_child_focus = 0;
    s_child_pids[idx] = pid;
    return 0;
}

static int live_children(void)
{
    int n = 0;
    for (int i = 0; i < CHILD_COUNT; i++)
        if (s_child_pids[i] > 0) n++;
    return n;
}

static int first_free_child_slot(void)
{
    for (int i = 0; i < CHILD_COUNT; i++)
        if (s_child_pids[i] <= 0) return i;
    return -1;
}

static int slen(const char *s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void put_cell(tty_cell_t *cells, unsigned int *n,
                     unsigned int col, unsigned int row, char ch,
                     unsigned char clr)
{
    cells[*n].col = (unsigned char)col;
    cells[*n].row = (unsigned char)row;
    cells[*n].ch = (unsigned char)ch;
    cells[*n].clr = clr;
    (*n)++;
}

static void put_cells_str(tty_cell_t *cells, unsigned int *n,
                          unsigned int col, unsigned int row,
                          const char *s, unsigned char clr)
{
    while (*s) {
        put_cell(cells, n, col++, row, *s++, clr);
    }
}

static int read_rtc(char *buf, unsigned int cap)
{
    int fd = sys_open("/proc/rtc", O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, cap - 1);
    sys_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void status_label(char *out)
{
    if (!s_clock_mode) {
        out[0] = 'M'; out[1] = 'a'; out[2] = 'k'; out[3] = 'a';
        out[4] = 'r'; out[5] = '\0';
        return;
    }

    char rtc[32];
    if (read_rtc(rtc, sizeof(rtc)) < 19) {
        out[0] = '-'; out[1] = '-'; out[2] = ':'; out[3] = '-';
        out[4] = '-'; out[5] = ':'; out[6] = '-'; out[7] = '-';
        out[8] = ' '; out[9] = '-'; out[10] = '-'; out[11] = '/';
        out[12] = '-'; out[13] = '-'; out[14] = '/'; out[15] = '-';
        out[16] = '-'; out[17] = '\0';
        return;
    }

    /* /proc/rtc is "YYYY-MM-DD HH:MM:SS"; status wants HH:MM:SS DD/MM/YY. */
    out[0] = rtc[11]; out[1] = rtc[12]; out[2] = ':';
    out[3] = rtc[14]; out[4] = rtc[15]; out[5] = ':';
    out[6] = rtc[17]; out[7] = rtc[18]; out[8] = ' ';
    out[9] = rtc[8]; out[10] = rtc[9]; out[11] = '/';
    out[12] = rtc[5]; out[13] = rtc[6]; out[14] = '/';
    out[15] = rtc[2]; out[16] = rtc[3]; out[17] = '\0';
}

static void draw_status(unsigned int state)
{
    unsigned int size = (unsigned int)sys_term_size();
    unsigned int cols = (size >> 16) & 0xFFFFu;
    unsigned int row = size & 0xFFFFu;
    if (cols == 0 || row == 0) return;

    unsigned int active = (state >> 16) & 0xFFFFu;
    unsigned int mask = state & 0xFFFFu;
    unsigned int count = 0;
    for (int i = 0; i < CHILD_COUNT; i++)
        if (mask & (1u << i)) count++;

    tty_cell_t cells[512];
    unsigned int n = 0;
    if (cols > sizeof(cells) / sizeof(cells[0]))
        cols = sizeof(cells) / sizeof(cells[0]);

    for (unsigned int c = 0; c < cols; c++)
        put_cell(cells, &n, c, row, ' ', STATUS_BAR_CLR);

    char label[20];
    status_label(label);
    put_cells_str(cells, &n, STATUS_LABEL_COL, row, label, STATUS_BAR_CLR);

    unsigned int label_end = STATUS_LABEL_COL + STATUS_LABEL_W;
    unsigned int block = count * 6u;
    unsigned int col = (cols > block) ? (cols - block) / 2u : label_end;
    if (col < label_end) col = label_end;
    for (int i = 0; i < CHILD_COUNT && col + 5 < cols; i++) {
        if (!(mask & (1u << i))) continue;
        char tab[6] = { ' ', 'V', 'T', (char)('1' + i), ' ', '\0' };
        put_cells_str(cells, &n, col, row, tab,
                      (i == (int)active) ? STATUS_ACTIVE_CLR : STATUS_BAR_CLR);
        col += 6;
    }

    const char *hint = "Alt+F1-F4";
    unsigned int hlen = (unsigned int)slen(hint);
    if (cols > hlen + 2)
        put_cells_str(cells, &n, cols - hlen - 1, row, hint, STATUS_BAR_CLR);

    sys_putch_at(cells, n);
}

static void note_child_exit(int pid)
{
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (s_child_pids[i] == pid) {
            s_child_pids[i] = 0;
            return;
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    put_s("makmux: starting mak.sh1-4\n");

    int ok = 1;
    for (int i = 0; i < CHILD_COUNT; i++) {
        if (spawn_child(i, 0) != 0) {
            put_s("makmux: fork failed for mak.sh");
            put_dec(i + 1);
            put_s("\n");
            ok = 0;
        }
    }

    put_s("makmux: use Alt+F1..Alt+F4 for mak.sh1..mak.sh4\n");
    if (!ok) put_s("makmux: one or more child shells failed to start\n");
    draw_status(sys_vt_state());

    for (;;) {
        int status = 0;
        int pid = sys_wait4(-1, &status, WNOHANG);
        if (pid > 0) {
            put_s("makmux: child shell exited pid=");
            put_dec(pid);
            put_s(" status=");
            put_dec(status);
            put_s("\n");
            sys_vt_close(pid);
            note_child_exit(pid);
            s_last_state = 0xFFFFFFFFu;
            if (live_children() == 0)
                return 0;
        }

        int opens = sys_vt_open_request();
        while (opens-- > 0 && live_children() < CHILD_COUNT) {
            int idx = first_free_child_slot();
            if (idx >= 0 && spawn_child(idx, 1) == 0)
                s_last_state = 0xFFFFFFFFu;
        }

        int toggles = sys_vt_clock_request();
        if (toggles > 0) {
            if (toggles & 1) s_clock_mode = !s_clock_mode;
            s_last_state = 0xFFFFFFFFu;
        }

        unsigned int state = sys_vt_state();
        unsigned int tick = (unsigned int)sys_uptime();
        if (state != s_last_state || (s_clock_mode && tick / 100u != s_last_tick / 100u)) {
            draw_status(state);
            s_last_state = state;
            s_last_tick = tick;
        }
        sys_yield();
    }
}
