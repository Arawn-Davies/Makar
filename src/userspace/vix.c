/*
 * vix.c - VIX interactive text editor (userland port).
 *
 * Ported from src/kernel/arch/i386/proc/vix.c.
 * All kernel calls replaced with int 0x80 syscalls.
 */

#include "syscall.h"

#define VFS_PATH_MAX   128
#define VGA_WIDTH       80

#define VIX_TEXT_ROWS  24
#define VIX_STATUS_ROW 24
#define VIX_MAX_LINES  256
#define VIX_LINE_CAP    80
#define VIX_FILE_MAX   (64u * 1024u)

/* VGA colour attributes */
#define VIX_CLR_TEXT    VGA_CLR(VGA_LGREY,    VGA_BLACK)
#define VIX_CLR_STATUS  VGA_CLR(VGA_BLACK,    VGA_LGREY)
#define VIX_CLR_WARN    VGA_CLR(VGA_LRED,     VGA_BLACK)
#define VIX_SHELL_CLR   VGA_CLR(VGA_WHITE,    VGA_BLUE)

/* Editor state */
static char v_lines[VIX_MAX_LINES][VIX_LINE_CAP + 1];
static int  v_len  [VIX_MAX_LINES];
static int  v_nlines;
static int  v_cur_row;
static int  v_cur_col;
static int  v_view_top;
static int  v_dirty;
static int  v_quit_warn;
static char v_path[VFS_PATH_MAX];

/* Cell batch buffer - flushed once per redraw. */
static tty_cell_t g_cells[VGA_WIDTH * (VIX_TEXT_ROWS + 1)];
static int        g_ncells;

static inline void vix_put(int col, int row, char c, unsigned char clr)
{
    if (g_ncells < (int)(sizeof(g_cells) / sizeof(g_cells[0]))) {
        g_cells[g_ncells].col = (unsigned char)col;
        g_cells[g_ncells].row = (unsigned char)row;
        g_cells[g_ncells].ch  = (unsigned char)c;
        g_cells[g_ncells].clr = clr;
        g_ncells++;
    }
}

static void vix_flush(void)
{
    if (g_ncells > 0) {
        sys_putch_at(g_cells, (unsigned int)g_ncells);
        g_ncells = 0;
    }
}

/* Simple unsigned-integer → decimal string (buf must be ≥ 12 bytes). */
static void vix_uitoa(unsigned int v, char *buf)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static unsigned int vix_strlen(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static void vix_append(char *buf, int cap, int *off, const char *s)
{
    while (*s && *off < cap - 1) buf[(*off)++] = *s++;
    buf[*off] = '\0';
}

static void vix_draw_line(int screen_row, int line_idx)
{
    const char *s   = (line_idx < v_nlines) ? v_lines[line_idx] : 0;
    int         len = s ? v_len[line_idx] : 0;

    for (int col = 0; col < VGA_WIDTH; col++) {
        char ch = (col < len) ? s[col] : ' ';
        vix_put(col, screen_row, ch, VIX_CLR_TEXT);
    }
}

static void vix_draw_status(void)
{
    char bar[VGA_WIDTH + 1];
    int  off = 0;

    if (v_quit_warn) {
        vix_append(bar, (int)sizeof(bar), &off,
                    " Unsaved! Press ^Q again to quit, or ^S to save. ");
    } else {
        vix_append(bar, (int)sizeof(bar), &off, " VIX | ");
        if (v_path[0])
            vix_append(bar, (int)sizeof(bar), &off, v_path);
        else
            vix_append(bar, (int)sizeof(bar), &off, "[new]");
        if (v_dirty) vix_append(bar, (int)sizeof(bar), &off, " *");
        vix_append(bar, (int)sizeof(bar), &off, " | Ln ");
        char num[12];
        vix_uitoa((unsigned int)(v_cur_row + 1), num);
        vix_append(bar, (int)sizeof(bar), &off, num);
        vix_append(bar, (int)sizeof(bar), &off, " Col ");
        vix_uitoa((unsigned int)(v_cur_col + 1), num);
        vix_append(bar, (int)sizeof(bar), &off, num);
        vix_append(bar, (int)sizeof(bar), &off, " | ^S save  ^Q quit");
    }

    unsigned int blen = vix_strlen(bar);
    while (blen < VGA_WIDTH) { bar[blen++] = ' '; }
    bar[VGA_WIDTH] = '\0';

    unsigned char clr = v_quit_warn ? VIX_CLR_WARN : VIX_CLR_STATUS;
    for (int col = 0; col < VGA_WIDTH; col++)
        vix_put(col, VIX_STATUS_ROW, bar[col], clr);
}

static void vix_redraw(void)
{
    g_ncells = 0;
    for (int r = 0; r < VIX_TEXT_ROWS; r++)
        vix_draw_line(r, v_view_top + r);
    vix_draw_status();
    vix_flush();
    sys_set_cursor((unsigned int)v_cur_col,
                   (unsigned int)(v_cur_row - v_view_top));
}

static void vix_clamp_col(void)
{
    int max = (v_cur_row < v_nlines) ? v_len[v_cur_row] : 0;
    if (v_cur_col > max) v_cur_col = max;
}

static void vix_scroll(void)
{
    if (v_cur_row < v_view_top)
        v_view_top = v_cur_row;
    else if (v_cur_row >= v_view_top + VIX_TEXT_ROWS)
        v_view_top = v_cur_row - VIX_TEXT_ROWS + 1;
}

static void vix_insert_char(char c)
{
    if (v_cur_row >= VIX_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0';
        v_len[v_nlines]       = 0;
        v_nlines++;
    }
    char *line = v_lines[v_cur_row];
    int   len  = v_len[v_cur_row];
    if (len >= VIX_LINE_CAP) return;
    for (int i = len; i > v_cur_col; i--)
        line[i] = line[i - 1];
    line[v_cur_col]  = c;
    line[len + 1]    = '\0';
    v_len[v_cur_row] = len + 1;
    v_cur_col++;
    v_dirty = 1;
}

static void vix_backspace(void)
{
    if (v_cur_row == 0 && v_cur_col == 0) return;
    if (v_cur_col > 0) {
        char *line = v_lines[v_cur_row];
        int   len  = v_len[v_cur_row];
        for (int i = v_cur_col - 1; i < len - 1; i++)
            line[i] = line[i + 1];
        line[len - 1]    = '\0';
        v_len[v_cur_row] = len - 1;
        v_cur_col--;
    } else {
        int prev     = v_cur_row - 1;
        int prev_len = v_len[prev];
        int cur_len  = v_len[v_cur_row];
        if (prev_len + cur_len > VIX_LINE_CAP) return;
        /* Manual memcpy */
        for (int i = 0; i <= cur_len; i++)
            v_lines[prev][prev_len + i] = v_lines[v_cur_row][i];
        v_len[prev] = prev_len + cur_len;
        for (int i = v_cur_row; i < v_nlines - 1; i++) {
            for (int j = 0; j <= v_len[i + 1]; j++)
                v_lines[i][j] = v_lines[i + 1][j];
            v_len[i] = v_len[i + 1];
        }
        v_nlines--;
        v_cur_row = prev;
        v_cur_col = prev_len;
    }
    v_dirty = 1;
}

static void vix_newline(void)
{
    if (v_nlines >= VIX_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0';
        v_len[v_nlines]       = 0;
        v_nlines++;
    }
    int cur_len   = v_len[v_cur_row];
    int right_len = cur_len - v_cur_col;
    for (int i = v_nlines; i > v_cur_row + 1; i--) {
        for (int j = 0; j <= v_len[i - 1]; j++)
            v_lines[i][j] = v_lines[i - 1][j];
        v_len[i] = v_len[i - 1];
    }
    for (int i = 0; i <= right_len; i++)
        v_lines[v_cur_row + 1][i] = v_lines[v_cur_row][v_cur_col + i];
    v_len[v_cur_row + 1] = right_len;
    v_lines[v_cur_row][v_cur_col] = '\0';
    v_len[v_cur_row]               = v_cur_col;
    v_nlines++;
    v_cur_row++;
    v_cur_col = 0;
    v_dirty   = 1;
}

static void vix_parse(const char *buf, unsigned int size)
{
    v_nlines = 0; v_cur_row = 0; v_cur_col = 0;
    v_view_top = 0; v_dirty = 0; v_quit_warn = 0;
    unsigned int pos = 0;
    while (v_nlines < VIX_MAX_LINES) {
        int out = 0;
        while (pos < size && buf[pos] != '\n') {
            if (buf[pos] == '\t') {
                int spaces = 4 - (out % 4);
                while (spaces-- > 0 && out < VIX_LINE_CAP)
                    v_lines[v_nlines][out++] = ' ';
            } else if (out < VIX_LINE_CAP) {
                v_lines[v_nlines][out++] = buf[pos];
            }
            pos++;
        }
        v_lines[v_nlines][out] = '\0';
        v_len[v_nlines]        = out;
        v_nlines++;
        if (pos >= size) break;
        pos++;
    }
    if (v_nlines == 0) { v_lines[0][0] = '\0'; v_len[0] = 0; v_nlines = 1; }
}

/* Flat file buffer for load/save (64 KiB static). */
static char v_filebuf[VIX_FILE_MAX];

static int vix_save(void)
{
    if (!v_path[0]) return -1;
    unsigned int off = 0;
    for (int i = 0; i < v_nlines; i++) {
        for (int j = 0; j < v_len[i] && off < VIX_FILE_MAX - 2; j++)
            v_filebuf[off++] = v_lines[i][j];
        if (i < v_nlines - 1 && off < VIX_FILE_MAX - 1)
            v_filebuf[off++] = '\n';
    }
    v_filebuf[off] = '\0';
    int err = sys_write_file(v_path, v_filebuf, off);
    if (err == 0) v_dirty = 0;
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        const char *msg = "Usage: vix <filename>\n";
        sys_write(1, msg, 23);
        sys_exit(1);
    }

    /* Store path. */
    const char *src = argv[1];
    int pi = 0;
    while (*src && pi < VFS_PATH_MAX - 1) v_path[pi++] = *src++;
    v_path[pi] = '\0';

    /* Load file. */
    int fd = sys_open(v_path, O_RDONLY);
    if (fd >= 0) {
        long got = sys_read(fd, v_filebuf, VIX_FILE_MAX - 1);
        sys_close(fd);
        if (got > 0) {
            v_filebuf[got] = '\0';
            vix_parse(v_filebuf, (unsigned int)got);
        } else {
            vix_parse("", 0);
        }
    } else {
        vix_parse("", 0);
    }

    /* Clear to editor background. */
    sys_tty_clear(VIX_CLR_TEXT);

    for (;;) {
        vix_scroll();
        vix_redraw();

        int c = sys_getkey();

        if (c == KEY_CTRL_Q) {
            if (!v_dirty || v_quit_warn) break;
            v_quit_warn = 1;
            continue;
        }
        v_quit_warn = 0;

        if (c == KEY_CTRL_S) {
            if (v_path[0]) vix_save();
            continue;
        }

        if (c == KEY_ARROW_UP) {
            if (v_cur_row > 0) { v_cur_row--; vix_clamp_col(); }
            continue;
        }
        if (c == KEY_ARROW_DOWN) {
            if (v_cur_row < v_nlines - 1) { v_cur_row++; vix_clamp_col(); }
            continue;
        }
        if (c == KEY_ARROW_LEFT) {
            if (v_cur_col > 0) {
                v_cur_col--;
            } else if (v_cur_row > 0) {
                v_cur_row--;
                v_cur_col = v_len[v_cur_row];
            }
            continue;
        }
        if (c == KEY_ARROW_RIGHT) {
            if (v_cur_col < v_len[v_cur_row]) {
                v_cur_col++;
            } else if (v_cur_row < v_nlines - 1) {
                v_cur_row++;
                v_cur_col = 0;
            }
            continue;
        }

        if (c == '\b')              { vix_backspace(); continue; }
        if (c == '\n' || c == '\r') { vix_newline();   continue; }
        if (c == '\t') {
            for (int s = 0; s < 4; s++) vix_insert_char(' ');
            continue;
        }
        if (c >= ' ' && c <= '~') { vix_insert_char((char)c); continue; }
    }

    /* Restore shell colour scheme. */
    sys_tty_clear(VIX_SHELL_CLR);
    return 0;
}
