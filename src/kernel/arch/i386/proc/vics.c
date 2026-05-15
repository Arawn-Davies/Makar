/*
 * vics.c - VICS interactive text editor for Makar.
 *
 * Renders into a vesa_pane_t so it works correctly at any VESA resolution.
 * Pass pane=NULL to vics_edit() and it falls back to the default full-screen
 * pane (or VGA dimensions when VESA is unavailable).
 */

#include <kernel/vics.h>
#include <kernel/vfs.h>
#include <kernel/vga.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/heap.h>
#include <kernel/vesa_tty.h>
#include <kernel/vtty.h>
#include <string.h>

#define VICS_MAX_LINES   256
#define VICS_LINE_CAP    80
#define VICS_FILE_MAX    (64u * 1024u)
#define VICS_STATUS_BUF  256

/* Left gutter: 4-digit right-aligned line number + 1 space separator.
 * VIM-style.  Reduces usable text width by this many columns. */
#define VICS_GUTTER_W    5

#define VICS_CLR_TEXT    0x07u
#define VICS_CLR_STATUS  0x70u
#define VICS_CLR_GUTTER  0x08u  /* dim grey on black */
#define VICS_CLR_WARN    make_color(COLOR_LIGHT_RED, COLOR_BLACK)
#define VICS_SHELL_VGA   0x1Fu
#define VICS_SHELL_FG    0xFFFFFFu
#define VICS_SHELL_BG    0x0000AAu
#define VICS_GUTTER_FG   0x808080u
#define VICS_GUTTER_BG   0x000000u
#define VICS_TEXT_FG     0xFFFFFFu
#define VICS_TEXT_BG     0x000000u

#define CTRL_S  '\x13'
#define CTRL_Q  '\x11'

/* Runtime screen geometry - set at entry from the active pane. */
static vesa_pane_t *v_pane;
static int v_cols;        /* total pane width */
static int v_text_cols;   /* v_cols - VICS_GUTTER_W: usable for content */
static int v_text_rows;   /* visible rows for content (status row excluded) */
static int v_status_row;  /* status bar's pane-relative row */

/* Editor buffer */
static char v_lines[VICS_MAX_LINES][VICS_LINE_CAP + 1];
static int  v_len  [VICS_MAX_LINES];
static int  v_nlines;
static int  v_cur_row;
static int  v_cur_col;
static int  v_view_top;
static int  v_dirty;
static int  v_quit_warn;
static int  v_save_msg;   /* 1 = "Saved", -1 = "Save failed", 0 = none */
static char v_path[VFS_PATH_MAX];

/* Write one character at pane-relative (col, row). */
static inline void vics_put(int col, int row, char c, uint8_t clr)
{
    int abs_row = (v_pane ? (int)v_pane->top_row : 0) + row;
    if (col < VGA_WIDTH && abs_row < 50)
        VGA_MEMORY[abs_row * VGA_WIDTH + col] = make_vgaentry(c, clr);
    if (vesa_tty_is_ready()) {
        if (v_pane)
            vesa_tty_pane_put_at(v_pane, c, (uint32_t)col, (uint32_t)row);
        else
            vesa_tty_put_at(c, (uint32_t)col, (uint32_t)abs_row);
    }
}

static void vics_uitoa(unsigned int v, char *buf);

/* How many visible rows a logical line occupies when wrap is on.  A
 * zero-length line still claims one row (so empty lines render an empty
 * gutter slot and remain navigable). */
static int line_vrows(int li)
{
    if (li < 0 || li >= v_nlines) return 1;
    int L = v_len[li];
    if (L == 0) return 1;
    int w = (v_text_cols > 0) ? v_text_cols : 1;
    return (L + w - 1) / w;
}

/* Render the left-gutter slot for one visible row.  `seg` is the wrap
 * segment index inside the logical line; only the first segment gets
 * the line number, continuation segments get a faint indent marker. */
static void vics_draw_gutter(int screen_row, int line_one_based, int seg)
{
    char gut[VICS_GUTTER_W + 1];
    /* Default: blank gutter. */
    for (int i = 0; i < VICS_GUTTER_W; i++) gut[i] = ' ';
    gut[VICS_GUTTER_W] = '\0';

    if (line_one_based > 0 && seg == 0) {
        /* Right-align the decimal number into the first 4 slots. */
        char tmp[12];
        vics_uitoa((unsigned int)line_one_based, tmp);
        int len = (int)strlen(tmp);
        if (len > 4) len = 4;
        int start = 4 - len;
        for (int i = 0; i < len; i++) gut[start + i] = tmp[i];
        gut[4] = ' ';
    } else if (line_one_based > 0 && seg > 0) {
        /* Continuation marker: a small '+' to show this row is the wrap
         * continuation of the line above.  Bash less calls these "fold". */
        gut[3] = '+';
        gut[4] = ' ';
    }

    if (vesa_tty_is_ready())
        vesa_tty_setcolor(VICS_GUTTER_FG, VICS_GUTTER_BG);
    for (int c = 0; c < VICS_GUTTER_W; c++)
        vics_put(c, screen_row, gut[c], VICS_CLR_GUTTER);
}

/* Render one visible row's worth of text content for a logical line,
 * pulling chars [seg_start .. seg_start+v_text_cols) and padding with
 * spaces past EOL. */
static void vics_draw_text(int screen_row, int line_idx, int seg_start)
{
    const char *s   = (line_idx >= 0 && line_idx < v_nlines) ? v_lines[line_idx] : NULL;
    int         len = s ? v_len[line_idx] : 0;

    if (vesa_tty_is_ready())
        vesa_tty_setcolor(VICS_TEXT_FG, VICS_TEXT_BG);

    for (int c = 0; c < v_text_cols; c++) {
        int idx = seg_start + c;
        char ch = (s && idx < len) ? s[idx] : ' ';
        vics_put(VICS_GUTTER_W + c, screen_row, ch, VICS_CLR_TEXT);
    }
}

static void vics_uitoa(unsigned int v, char *buf)
{
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[12]; int i = 0;
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void vics_append(char *buf, int cap, int *off, const char *s)
{
    while (*s && *off < cap - 1) buf[(*off)++] = *s++;
    buf[*off] = '\0';
}

static void vics_draw_status(void)
{
    char bar[VICS_STATUS_BUF];
    int  off = 0;
    int  cap = (v_cols < VICS_STATUS_BUF - 1) ? v_cols + 1 : VICS_STATUS_BUF;

    if (v_quit_warn) {
        vics_append(bar, cap, &off, " Unsaved changes! ^Q to discard, ^S to save. ");
    } else if (v_save_msg == 1) {
        vics_append(bar, cap, &off, " Saved. | ");
        vics_append(bar, cap, &off, v_path[0] ? v_path : "[new]");
        vics_append(bar, cap, &off, " | ^S:Save  ^Q:Quit");
    } else if (v_save_msg == -1) {
        vics_append(bar, cap, &off, " Save failed! | ^S:Retry  ^Q:Quit");
    } else {
        vics_append(bar, cap, &off, " VICS | ");
        vics_append(bar, cap, &off, v_path[0] ? v_path : "[new]");
        if (v_dirty) vics_append(bar, cap, &off, " [+]");
        vics_append(bar, cap, &off, " | Ln ");
        char tmp[12];
        vics_uitoa((unsigned int)(v_cur_row + 1), tmp);
        vics_append(bar, cap, &off, tmp);
        vics_append(bar, cap, &off, "/");
        vics_uitoa((unsigned int)v_nlines, tmp);
        vics_append(bar, cap, &off, tmp);
        vics_append(bar, cap, &off, " | ^S:Save  ^Q:Quit");
    }

    while (off < v_cols) bar[off++] = ' ';
    bar[off] = '\0';

    uint8_t clr = (v_quit_warn || v_save_msg == -1) ? VICS_CLR_WARN : VICS_CLR_STATUS;

    if (vesa_tty_is_ready()) {
        if (v_quit_warn || v_save_msg == -1) vesa_tty_setcolor(0xFF0000u, 0x000000u);
        else if (v_save_msg == 1)            vesa_tty_setcolor(0x00FF00u, 0x000000u);
        else                                 vesa_tty_setcolor(0x000000u, 0xAAAAAAu);
    }

    for (int col = 0; col < v_cols; col++)
        vics_put(col, v_status_row, bar[col], clr);
}

/* Compute the visible row offset of the cursor relative to v_view_top,
 * accounting for wrap of every line in between. */
static int vics_cursor_vrow(void)
{
    int vr = 0;
    for (int i = v_view_top; i < v_cur_row && i < v_nlines; i++)
        vr += line_vrows(i);
    return vr + (v_cur_col / (v_text_cols > 0 ? v_text_cols : 1));
}

static void vics_redraw(void)
{
    int vrow = 0;
    int li   = v_view_top;

    while (vrow < v_text_rows && li < v_nlines) {
        int segs = line_vrows(li);
        for (int seg = 0; seg < segs && vrow < v_text_rows; seg++) {
            vics_draw_gutter(vrow, li + 1, seg);
            vics_draw_text(vrow, li, seg * v_text_cols);
            vrow++;
        }
        li++;
    }

    /* Vim-style empty-buffer indicator on lines past EOF: blank gutter,
     * '~' in the first text column. */
    while (vrow < v_text_rows) {
        vics_draw_gutter(vrow, 0, 0);
        if (vesa_tty_is_ready())
            vesa_tty_setcolor(VICS_GUTTER_FG, VICS_TEXT_BG);
        vics_put(VICS_GUTTER_W, vrow, '~', VICS_CLR_GUTTER);
        for (int c = VICS_GUTTER_W + 1; c < v_cols; c++)
            vics_put(c, vrow, ' ', VICS_CLR_TEXT);
        vrow++;
    }

    vics_draw_status();

    /* Position the cursor at the wrap-adjusted visible row + column. */
    int cur_vrow  = vics_cursor_vrow();
    int cur_inseg = v_cur_col % (v_text_cols > 0 ? v_text_cols : 1);
    int abs_row   = (v_pane ? (int)v_pane->top_row : 0) + cur_vrow;
    update_cursor((size_t)abs_row, (size_t)(VICS_GUTTER_W + cur_inseg));
    if (vesa_tty_is_ready() && v_pane)
        vesa_tty_pane_set_cursor(v_pane,
                                 (uint32_t)(VICS_GUTTER_W + cur_inseg),
                                 (uint32_t)cur_vrow);
}

static void vics_clamp_col(void)
{
    int max = (v_cur_row < v_nlines) ? v_len[v_cur_row] : 0;
    if (v_cur_col > max) v_cur_col = max;
}

/* Keep the cursor visible.  With wrap, the cursor's visible row is the
 * sum of v_view_top..cur_row line heights plus the in-line segment, so
 * scrolling has to advance v_view_top until that sum fits. */
static void vics_scroll(void)
{
    if (v_cur_row < v_view_top) {
        v_view_top = v_cur_row;
        return;
    }
    while (vics_cursor_vrow() >= v_text_rows && v_view_top < v_cur_row)
        v_view_top++;
}

static void vics_insert_char(char c)
{
    if (v_cur_row >= VICS_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0'; v_len[v_nlines] = 0; v_nlines++;
    }
    char *line = v_lines[v_cur_row];
    int   len  = v_len[v_cur_row];
    if (len >= VICS_LINE_CAP) return;
    for (int i = len; i > v_cur_col; i--) line[i] = line[i - 1];
    line[v_cur_col] = c; line[len + 1] = '\0';
    v_len[v_cur_row] = len + 1; v_cur_col++; v_dirty = 1;
}

static void vics_backspace(void)
{
    if (v_cur_row == 0 && v_cur_col == 0) return;
    if (v_cur_col > 0) {
        char *line = v_lines[v_cur_row]; int len = v_len[v_cur_row];
        for (int i = v_cur_col - 1; i < len - 1; i++) line[i] = line[i + 1];
        line[len - 1] = '\0'; v_len[v_cur_row] = len - 1; v_cur_col--;
    } else {
        int prev = v_cur_row - 1, prev_len = v_len[prev], cur_len = v_len[v_cur_row];
        if (prev_len + cur_len > VICS_LINE_CAP) return;
        memcpy(v_lines[prev] + prev_len, v_lines[v_cur_row], (size_t)(cur_len + 1));
        v_len[prev] = prev_len + cur_len;
        for (int i = v_cur_row; i < v_nlines - 1; i++) {
            memcpy(v_lines[i], v_lines[i + 1], (size_t)(v_len[i + 1] + 1));
            v_len[i] = v_len[i + 1];
        }
        v_nlines--; v_cur_row = prev; v_cur_col = prev_len;
    }
    v_dirty = 1;
}

static void vics_newline(void)
{
    if (v_nlines >= VICS_MAX_LINES) return;
    while (v_nlines <= v_cur_row) {
        v_lines[v_nlines][0] = '\0'; v_len[v_nlines] = 0; v_nlines++;
    }
    int cur_len = v_len[v_cur_row], right_len = cur_len - v_cur_col;
    for (int i = v_nlines; i > v_cur_row + 1; i--) {
        memcpy(v_lines[i], v_lines[i - 1], (size_t)(v_len[i - 1] + 1));
        v_len[i] = v_len[i - 1];
    }
    memcpy(v_lines[v_cur_row + 1], v_lines[v_cur_row] + v_cur_col, (size_t)(right_len + 1));
    v_len[v_cur_row + 1] = right_len;
    v_lines[v_cur_row][v_cur_col] = '\0'; v_len[v_cur_row] = v_cur_col;
    v_nlines++; v_cur_row++; v_cur_col = 0; v_dirty = 1;
}

static void vics_parse(const char *buf, uint32_t size)
{
    v_nlines = 0; v_cur_row = 0; v_cur_col = 0;
    v_view_top = 0; v_dirty = 0; v_quit_warn = 0;
    uint32_t pos = 0;
    while (v_nlines < VICS_MAX_LINES) {
        int out = 0;
        while (pos < size && buf[pos] != '\n') {
            if (buf[pos] == '\t') {
                int sp = 4 - (out % 4);
                while (sp-- > 0 && out < VICS_LINE_CAP) v_lines[v_nlines][out++] = ' ';
            } else if (out < VICS_LINE_CAP) {
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

static char *vics_flatten(uint32_t *out_size)
{
    uint32_t total = 0;
    for (int i = 0; i < v_nlines; i++) total += (uint32_t)v_len[i] + 1u;
    if (total > 0) total--;
    char *buf = (char *)kmalloc(total + 1u);
    if (!buf) return NULL;
    uint32_t off = 0;
    for (int i = 0; i < v_nlines; i++) {
        memcpy(buf + off, v_lines[i], (size_t)v_len[i]);
        off += (uint32_t)v_len[i];
        if (i < v_nlines - 1) buf[off++] = '\n';
    }
    buf[off] = '\0'; *out_size = off;
    return buf;
}

static int vics_save(void)
{
    if (!v_path[0]) return -1;
    uint32_t size = 0;
    char *buf = vics_flatten(&size);
    if (!buf) return -1;
    int err = vfs_write_file(v_path, buf, size);
    kfree(buf);
    if (err == 0) v_dirty = 0;
    return err;
}

void vics_edit(const char *path, vesa_pane_t *pane)
{
    v_pane = pane ? pane : (vesa_tty_is_ready() ? vesa_tty_default_pane() : NULL);

    /* Vim-style block caret while editing.  The default underscore is a
     * 2-px sliver that disappears against the dark text background. */
    uint32_t saved_caret = vesa_tty_is_ready() ? vesa_tty_get_caret_style() : 0;
    if (vesa_tty_is_ready())
        vesa_tty_set_caret_style(1);

    if (v_pane && vesa_tty_is_ready()) {
        v_cols       = (int)v_pane->cols;
        v_text_rows  = (int)v_pane->rows - 1;
        v_status_row = (int)v_pane->rows - 1;
    } else {
        v_cols       = VGA_WIDTH;
        v_text_rows  = 49;   /* 50-row VGA, leave last for status */
        v_status_row = 49;
    }
    /* Derive text-area width after subtracting the gutter.  Guard against
     * absurdly narrow panes (1-2 cols) by falling back to no gutter. */
    if (v_cols > VICS_GUTTER_W + 1)
        v_text_cols = v_cols - VICS_GUTTER_W;
    else
        v_text_cols = v_cols;
    if (v_text_cols > VICS_LINE_CAP) v_text_cols = VICS_LINE_CAP;

    if (path && *path) {
        strncpy(v_path, path, VFS_PATH_MAX - 1);
        v_path[VFS_PATH_MAX - 1] = '\0';
    } else {
        v_path[0] = '\0';
    }

    uint8_t *file_buf = (uint8_t *)kmalloc(VICS_FILE_MAX);
    if (file_buf) {
        uint32_t got = 0;
        if (v_path[0] &&
            vfs_read_file(v_path, file_buf, VICS_FILE_MAX, &got) == 0 && got > 0)
            vics_parse((const char *)file_buf, got);
        else
            vics_parse("", 0);
        kfree(file_buf);
    } else {
        vics_parse("", 0);
    }

    for (;;) {
        vics_scroll();
        vics_redraw();
        v_save_msg = 0;

        unsigned char c = keyboard_getchar();

        if (c == CTRL_Q) {
            if (!v_dirty || v_quit_warn) break;
            v_quit_warn = 1; continue;
        }
        v_quit_warn = 0;

        if (c == CTRL_S) {
            if (v_path[0]) v_save_msg = (vics_save() == 0) ? 1 : -1;
            else           v_save_msg = -1;
            continue;
        }

        if (c == KEY_ARROW_UP)    { if (v_cur_row > 0) { v_cur_row--; vics_clamp_col(); } continue; }
        if (c == KEY_ARROW_DOWN)  { if (v_cur_row < v_nlines - 1) { v_cur_row++; vics_clamp_col(); } continue; }
        if (c == KEY_ARROW_LEFT)  {
            if (v_cur_col > 0) v_cur_col--;
            else if (v_cur_row > 0) { v_cur_row--; v_cur_col = v_len[v_cur_row]; }
            continue;
        }
        if (c == KEY_ARROW_RIGHT) {
            if (v_cur_col < v_len[v_cur_row]) v_cur_col++;
            else if (v_cur_row < v_nlines - 1) { v_cur_row++; v_cur_col = 0; }
            continue;
        }

        if (c == '\b')              { vics_backspace(); continue; }
        if (c == '\n' || c == '\r') { vics_newline();   continue; }
        if (c == '\t') { for (int s = 0; s < 4; s++) vics_insert_char(' '); continue; }
        if (c >= ' ' && c <= '~')   { vics_insert_char(c); continue; }
    }

    terminal_set_colorscheme(VICS_SHELL_VGA);
    if (vesa_tty_is_ready()) {
        vesa_tty_setcolor(VICS_SHELL_FG, VICS_SHELL_BG);
        if (v_pane) vesa_tty_pane_clear(v_pane);
        else        vesa_tty_clear();
        /* Repaint the global status bar immediately - the pane clear
         * above only covers the text area, but VICS may have written
         * stale ink to the status row before this commit shrunk the
         * default pane.  Doing it here means the user sees the bar
         * the moment we return to the shell, not on the next Alt+Fn. */
        vesa_tty_paint_status(vtty_active(), vtty_count());
        vesa_tty_set_caret_style(saved_caret);
    }
}
