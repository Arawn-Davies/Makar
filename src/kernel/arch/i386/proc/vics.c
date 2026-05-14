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
#include <string.h>

#define VICS_MAX_LINES   256
#define VICS_LINE_CAP    80
#define VICS_FILE_MAX    (64u * 1024u)
#define VICS_STATUS_BUF  256

#define VICS_CLR_TEXT    0x07u
#define VICS_CLR_STATUS  0x70u
#define VICS_CLR_WARN    make_color(COLOR_LIGHT_RED, COLOR_BLACK)
#define VICS_SHELL_VGA   0x1Fu
#define VICS_SHELL_FG    0xFFFFFFu
#define VICS_SHELL_BG    0x0000AAu

#define CTRL_S  '\x13'
#define CTRL_Q  '\x11'

/* Runtime screen geometry - set at entry from the active pane. */
static vesa_pane_t *v_pane;
static int v_cols;
static int v_text_rows;
static int v_status_row;

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

static void vics_draw_line(int screen_row, int line_idx)
{
    const char *s   = (line_idx < v_nlines) ? v_lines[line_idx] : NULL;
    int         len = s ? v_len[line_idx] : 0;

    if (vesa_tty_is_ready())
        vesa_tty_setcolor(0xFFFFFFu, 0x000000u);

    for (int col = 0; col < v_cols; col++) {
        char ch = (col < len) ? s[col] : ' ';
        vics_put(col, screen_row, ch, VICS_CLR_TEXT);
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

static void vics_redraw(void)
{
    for (int r = 0; r < v_text_rows; r++)
        vics_draw_line(r, v_view_top + r);
    vics_draw_status();
    int abs_row = (v_pane ? (int)v_pane->top_row : 0) + (v_cur_row - v_view_top);
    update_cursor((size_t)abs_row, (size_t)v_cur_col);
}

static void vics_clamp_col(void)
{
    int max = (v_cur_row < v_nlines) ? v_len[v_cur_row] : 0;
    if (v_cur_col > max) v_cur_col = max;
}

static void vics_scroll(void)
{
    if (v_cur_row < v_view_top)
        v_view_top = v_cur_row;
    else if (v_cur_row >= v_view_top + v_text_rows)
        v_view_top = v_cur_row - v_text_rows + 1;
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

    if (v_pane && vesa_tty_is_ready()) {
        v_cols       = (int)v_pane->cols;
        v_text_rows  = (int)v_pane->rows - 1;
        v_status_row = (int)v_pane->rows - 1;
    } else {
        v_cols       = VGA_WIDTH;
        v_text_rows  = 49;   /* 50-row VGA, leave last for status */
        v_status_row = 49;
    }
    if (v_cols > VICS_LINE_CAP) v_cols = VICS_LINE_CAP;

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
    }
}
