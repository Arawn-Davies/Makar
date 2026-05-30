/*
 * vix.c - VIX interactive text editor (userland).
 *
 * Ports the polished in-kernel editor to ring 3: a vim-style left gutter
 * with line numbers, word wrap with continuation markers, a flashing
 * block caret, and resolution-agnostic geometry derived from the kernel
 * pane (sys_term_cols/rows).  All kernel calls go through int 0x80.
 *
 * Runs as its own task (reached via PATH, like clock/maktop), so it shows
 * up in maktop with its own pid + memory footprint.
 */

#include "syscall.h"

#define VFS_PATH_MAX   128

#define VIX_MAX_LINES  256
#define VIX_LINE_CAP    80
#define VIX_FILE_MAX   (64u * 1024u)
#define VIX_STATUS_BUF 256

/* Generous cell-buffer bounds; everything is clipped to the runtime
 * pane size reported by sys_term_cols/rows. */
#define VIX_MAX_COLS   160
#define VIX_MAX_ROWS   64

/* Left gutter: 4-digit right-aligned line number + 1 space separator. */
#define VIX_GUTTER_W    5

/* VGA colour attributes (sys_putch_at carries an attribute byte). */
#define VIX_CLR_TEXT    VGA_CLR(VGA_LGREY, VGA_BLACK)
#define VIX_CLR_GUTTER  VGA_CLR(VGA_DGREY, VGA_BLACK)
#define VIX_CLR_STATUS  VGA_CLR(VGA_BLACK, VGA_LGREY)
#define VIX_CLR_WARN    VGA_CLR(VGA_WHITE, VGA_RED)
#define VIX_CLR_SAVED   VGA_CLR(VGA_BLACK, VGA_LGREEN)

#define CTRL_S  '\x13'
#define CTRL_Q  '\x11'

/* Runtime geometry (set in main from the pane). */
static int v_cols;        /* total pane width                       */
static int v_text_cols;   /* usable content width = v_cols - gutter */
static int v_text_rows;   /* visible content rows (status excluded) */
static int v_status_row;  /* status-bar row                         */

/* Editor buffer. */
static char v_lines[VIX_MAX_LINES][VIX_LINE_CAP + 1];
static int  v_len  [VIX_MAX_LINES];
static int  v_nlines;
static int  v_cur_row;
static int  v_cur_col;
static int  v_view_top;
static int  v_dirty;
static int  v_quit_warn;
static int  v_save_msg;   /* 1 = "Saved", -1 = "Save failed", 0 = none */
static char v_path[VFS_PATH_MAX];

/* Cell batch buffer - flushed once per redraw. */
static tty_cell_t g_cells[VIX_MAX_COLS * VIX_MAX_ROWS];
static int        g_ncells;

static inline void vix_put(int col, int row, char c, unsigned char clr)
{
    if (col < 0 || col >= v_cols) return;
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

/* Visible rows a logical line occupies with wrap (>=1). */
static int line_vrows(int li)
{
    if (li < 0 || li >= v_nlines) return 1;
    int L = v_len[li];
    if (L == 0) return 1;
    int w = (v_text_cols > 0) ? v_text_cols : 1;
    return (L + w - 1) / w;
}

/* Left-gutter slot: line number on the first wrap segment, a '+'
 * continuation marker on later segments. */
static void vix_draw_gutter(int screen_row, int line_one_based, int seg)
{
    char gut[VIX_GUTTER_W + 1];
    for (int i = 0; i < VIX_GUTTER_W; i++) gut[i] = ' ';
    gut[VIX_GUTTER_W] = '\0';

    if (line_one_based > 0 && seg == 0) {
        char tmp[12];
        vix_uitoa((unsigned int)line_one_based, tmp);
        int len = (int)vix_strlen(tmp);
        if (len > 4) len = 4;
        int start = 4 - len;
        for (int i = 0; i < len; i++) gut[start + i] = tmp[i];
        gut[4] = ' ';
    } else if (line_one_based > 0 && seg > 0) {
        gut[3] = '+';
        gut[4] = ' ';
    }

    for (int c = 0; c < VIX_GUTTER_W; c++)
        vix_put(c, screen_row, gut[c], VIX_CLR_GUTTER);
}

/* One visible row of content for a logical line, chars
 * [seg_start .. seg_start+v_text_cols), space-padded to the pane edge. */
static void vix_draw_text(int screen_row, int line_idx, int seg_start)
{
    const char *s   = (line_idx >= 0 && line_idx < v_nlines) ? v_lines[line_idx] : 0;
    int         len = s ? v_len[line_idx] : 0;

    for (int c = 0; c < v_text_cols; c++) {
        int  idx = seg_start + c;
        char ch  = (s && idx < len) ? s[idx] : ' ';
        vix_put(VIX_GUTTER_W + c, screen_row, ch, VIX_CLR_TEXT);
    }
    for (int c = VIX_GUTTER_W + v_text_cols; c < v_cols; c++)
        vix_put(c, screen_row, ' ', VIX_CLR_TEXT);
}

static void vix_draw_status(void)
{
    char bar[VIX_STATUS_BUF];
    int  off = 0;
    int  cap = (v_cols < VIX_STATUS_BUF - 1) ? v_cols + 1 : VIX_STATUS_BUF;

    if (v_quit_warn) {
        vix_append(bar, cap, &off, " Unsaved changes! ^Q to discard, ^S to save. ");
    } else if (v_save_msg == 1) {
        vix_append(bar, cap, &off, " Saved. | ");
        vix_append(bar, cap, &off, v_path[0] ? v_path : "[new]");
        vix_append(bar, cap, &off, " | ^S:Save  ^Q:Quit");
    } else if (v_save_msg == -1) {
        vix_append(bar, cap, &off, " Save failed! | ^S:Retry  ^Q:Quit");
    } else {
        vix_append(bar, cap, &off, " VIX | ");
        vix_append(bar, cap, &off, v_path[0] ? v_path : "[new]");
        if (v_dirty) vix_append(bar, cap, &off, " [+]");
        vix_append(bar, cap, &off, " | Ln ");
        char tmp[12];
        vix_uitoa((unsigned int)(v_cur_row + 1), tmp);
        vix_append(bar, cap, &off, tmp);
        vix_append(bar, cap, &off, "/");
        vix_uitoa((unsigned int)v_nlines, tmp);
        vix_append(bar, cap, &off, tmp);
        vix_append(bar, cap, &off, " | ^S:Save  ^Q:Quit");
    }

    while (off < v_cols && off < VIX_STATUS_BUF - 1) bar[off++] = ' ';
    bar[off] = '\0';

    unsigned char clr = VIX_CLR_STATUS;
    if (v_quit_warn || v_save_msg == -1) clr = VIX_CLR_WARN;
    else if (v_save_msg == 1)            clr = VIX_CLR_SAVED;

    for (int col = 0; col < v_cols; col++)
        vix_put(col, v_status_row, bar[col], clr);
}

/* Visible row offset of the cursor relative to v_view_top, wrap-aware. */
static int vix_cursor_vrow(void)
{
    int vr = 0;
    for (int i = v_view_top; i < v_cur_row && i < v_nlines; i++)
        vr += line_vrows(i);
    return vr + (v_cur_col / (v_text_cols > 0 ? v_text_cols : 1));
}

static void vix_redraw(void)
{
    g_ncells = 0;

    int vrow = 0;
    int li   = v_view_top;
    while (vrow < v_text_rows && li < v_nlines) {
        int segs = line_vrows(li);
        for (int seg = 0; seg < segs && vrow < v_text_rows; seg++) {
            vix_draw_gutter(vrow, li + 1, seg);
            vix_draw_text(vrow, li, seg * v_text_cols);
            vrow++;
        }
        li++;
    }
    /* Vim-style empty rows past EOF: blank gutter + '~'. */
    while (vrow < v_text_rows) {
        vix_draw_gutter(vrow, 0, 0);
        vix_put(VIX_GUTTER_W, vrow, '~', VIX_CLR_GUTTER);
        for (int c = VIX_GUTTER_W + 1; c < v_cols; c++)
            vix_put(c, vrow, ' ', VIX_CLR_TEXT);
        vrow++;
    }

    vix_draw_status();
    vix_flush();

    int cur_vrow  = vix_cursor_vrow();
    int cur_inseg = v_cur_col % (v_text_cols > 0 ? v_text_cols : 1);
    sys_set_cursor((unsigned int)(VIX_GUTTER_W + cur_inseg),
                   (unsigned int)cur_vrow);
}

static void vix_clamp_col(void)
{
    int max = (v_cur_row < v_nlines) ? v_len[v_cur_row] : 0;
    if (v_cur_col > max) v_cur_col = max;
}

static void vix_page_move(int dir)
{
    int budget = v_text_rows > 1 ? v_text_rows - 1 : 1;
    int row = v_cur_row;

    while (budget > 0) {
        int next = row + dir;
        if (next < 0 || next >= v_nlines) break;
        row = next;
        budget -= line_vrows(row);
    }

    v_cur_row = row;
    vix_clamp_col();
}

static void vix_scroll(void)
{
    if (v_cur_row < v_view_top) { v_view_top = v_cur_row; return; }
    while (vix_cursor_vrow() >= v_text_rows && v_view_top < v_cur_row)
        v_view_top++;
}

static void vix_insert_char(char c)
{
    if (v_cur_row >= VIX_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0'; v_len[v_nlines] = 0; v_nlines++;
    }
    char *line = v_lines[v_cur_row];
    int   len  = v_len[v_cur_row];
    if (len >= VIX_LINE_CAP) return;
    for (int i = len; i > v_cur_col; i--) line[i] = line[i - 1];
    line[v_cur_col] = c; line[len + 1] = '\0';
    v_len[v_cur_row] = len + 1; v_cur_col++; v_dirty = 1;
}

static void vix_backspace(void)
{
    if (v_cur_row == 0 && v_cur_col == 0) return;
    if (v_cur_col > 0) {
        char *line = v_lines[v_cur_row]; int len = v_len[v_cur_row];
        for (int i = v_cur_col - 1; i < len - 1; i++) line[i] = line[i + 1];
        line[len - 1] = '\0'; v_len[v_cur_row] = len - 1; v_cur_col--;
    } else {
        int prev = v_cur_row - 1, prev_len = v_len[prev], cur_len = v_len[v_cur_row];
        if (prev_len + cur_len > VIX_LINE_CAP) return;
        for (int i = 0; i <= cur_len; i++)
            v_lines[prev][prev_len + i] = v_lines[v_cur_row][i];
        v_len[prev] = prev_len + cur_len;
        for (int i = v_cur_row; i < v_nlines - 1; i++) {
            for (int j = 0; j <= v_len[i + 1]; j++) v_lines[i][j] = v_lines[i + 1][j];
            v_len[i] = v_len[i + 1];
        }
        v_nlines--; v_cur_row = prev; v_cur_col = prev_len;
    }
    v_dirty = 1;
}

static void vix_newline(void)
{
    if (v_nlines >= VIX_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0'; v_len[v_nlines] = 0; v_nlines++;
    }
    int cur_len = v_len[v_cur_row], right_len = cur_len - v_cur_col;
    for (int i = v_nlines; i > v_cur_row + 1; i--) {
        for (int j = 0; j <= v_len[i - 1]; j++) v_lines[i][j] = v_lines[i - 1][j];
        v_len[i] = v_len[i - 1];
    }
    for (int i = 0; i <= right_len; i++)
        v_lines[v_cur_row + 1][i] = v_lines[v_cur_row][v_cur_col + i];
    v_len[v_cur_row + 1] = right_len;
    v_lines[v_cur_row][v_cur_col] = '\0'; v_len[v_cur_row] = v_cur_col;
    v_nlines++; v_cur_row++; v_cur_col = 0; v_dirty = 1;
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
                int sp = 4 - (out % 4);
                while (sp-- > 0 && out < VIX_LINE_CAP) v_lines[v_nlines][out++] = ' ';
            } else if (out < VIX_LINE_CAP) {
                v_lines[v_nlines][out++] = buf[pos];
            }
            pos++;
        }
        v_lines[v_nlines][out] = '\0'; v_len[v_nlines] = out; v_nlines++;
        if (pos >= size) break;
        pos++;
    }
    if (v_nlines == 0) { v_lines[0][0] = '\0'; v_len[0] = 0; v_nlines = 1; }
}

static char v_filebuf[VIX_FILE_MAX];

static int vix_save(void)
{
    if (!v_path[0]) return -1;
    unsigned int off = 0;
    for (int i = 0; i < v_nlines; i++) {
        for (int j = 0; j < v_len[i] && off < VIX_FILE_MAX - 2; j++)
            v_filebuf[off++] = v_lines[i][j];
        if (i < v_nlines - 1 && off < VIX_FILE_MAX - 1) v_filebuf[off++] = '\n';
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
        sys_write(1, msg, vix_strlen(msg));
        sys_exit(1);
    }

    /* Resolution-agnostic geometry from the pane. */
    v_cols       = (int)sys_term_cols();
    int rows     = (int)sys_term_rows();
    if (v_cols <= 0)  v_cols = 80;
    if (v_cols > VIX_MAX_COLS) v_cols = VIX_MAX_COLS;
    if (rows  <= 1)   rows = 25;
    if (rows  > VIX_MAX_ROWS)  rows = VIX_MAX_ROWS;
    v_text_rows  = rows - 1;
    v_status_row = rows - 1;
    v_text_cols  = (v_cols > VIX_GUTTER_W + 1) ? (v_cols - VIX_GUTTER_W) : v_cols;
    if (v_text_cols > VIX_LINE_CAP) v_text_cols = VIX_LINE_CAP;

    /* Store path. */
    const char *src = argv[1];
    int pi = 0;
    while (*src && pi < VFS_PATH_MAX - 1) v_path[pi++] = *src++;
    v_path[pi] = '\0';

    /* Load file (empty buffer if it doesn't exist yet). */
    int fd = sys_open(v_path, O_RDONLY);
    if (fd >= 0) {
        long got = sys_read(fd, v_filebuf, VIX_FILE_MAX - 1);
        sys_close(fd);
        if (got > 0) { v_filebuf[got] = '\0'; vix_parse(v_filebuf, (unsigned int)got); }
        else         vix_parse("", 0);
    } else {
        vix_parse("", 0);
    }

    /* Flashing block caret while editing; restore on exit. */
    unsigned int saved_caret = sys_set_caret_style(2);

    sys_tty_clear(VIX_CLR_TEXT);

    for (;;) {
        vix_scroll();
        vix_redraw();
        v_save_msg = 0;

        int c = sys_getkey();

        if (c == KEY_CTRL_Q || c == CTRL_Q) {
            if (!v_dirty || v_quit_warn) break;
            v_quit_warn = 1; continue;
        }
        v_quit_warn = 0;

        if (c == KEY_CTRL_S || c == CTRL_S) {
            if (v_path[0]) v_save_msg = (vix_save() == 0) ? 1 : -1;
            else           v_save_msg = -1;
            continue;
        }

        if (c == KEY_ARROW_UP)   { if (v_cur_row > 0) { v_cur_row--; vix_clamp_col(); } continue; }
        if (c == KEY_ARROW_DOWN) { if (v_cur_row < v_nlines - 1) { v_cur_row++; vix_clamp_col(); } continue; }
        if (c == KEY_PAGE_UP)    { vix_page_move(-1); continue; }
        if (c == KEY_PAGE_DOWN)  { vix_page_move(1);  continue; }
        if (c == KEY_ARROW_LEFT) {
            if (v_cur_col > 0) v_cur_col--;
            else if (v_cur_row > 0) { v_cur_row--; v_cur_col = v_len[v_cur_row]; }
            continue;
        }
        if (c == KEY_ARROW_RIGHT) {
            if (v_cur_col < v_len[v_cur_row]) v_cur_col++;
            else if (v_cur_row < v_nlines - 1) { v_cur_row++; v_cur_col = 0; }
            continue;
        }

        if (c == '\b')              { vix_backspace(); continue; }
        if (c == '\n' || c == '\r') { vix_newline();   continue; }
        if (c == '\t') { for (int s = 0; s < 4; s++) vix_insert_char(' '); continue; }
        if (c >= ' ' && c <= '~')   { vix_insert_char((char)c); continue; }
    }

    sys_set_caret_style(saved_caret);
    /* shell_restore_screen reapplies the per-VT palette + backing grid. */
    return 0;
}
