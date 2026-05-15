/*
 * vt.c - virtual console backing grid.  See kernel/vt.h.
 */

#include <kernel/vt.h>
#include <stdlib.h>
#include <string.h>

static void vt_fill_cell(vt_cell_t *c, uint8_t ch, uint32_t fg, uint32_t bg)
{
    c->ch    = ch;
    c->flags = 0;
    c->fg    = fg;
    c->bg    = bg;
}

bool vt_init(vt_buf_t *vt, uint32_t cols, uint32_t rows,
             uint32_t default_fg, uint32_t default_bg)
{
    vt->cols    = cols;
    vt->rows    = rows;
    vt->cur_col = 0;
    vt->cur_row = 0;
    vt->fg      = default_fg;
    vt->bg      = default_bg;

    size_t n = (size_t)cols * (size_t)rows;
    vt->cells = (vt_cell_t *)malloc(n * sizeof(vt_cell_t));
    if (!vt->cells) return false;

    for (size_t i = 0; i < n; i++)
        vt_fill_cell(&vt->cells[i], ' ', default_fg, default_bg);
    return true;
}

void vt_set_color(vt_buf_t *vt, uint32_t fg, uint32_t bg)
{
    vt->fg = fg;
    vt->bg = bg;
}

void vt_set_cursor(vt_buf_t *vt, uint32_t col, uint32_t row)
{
    if (vt->cols == 0 || vt->rows == 0) return;
    if (col >= vt->cols) col = vt->cols - 1;
    if (row >= vt->rows) row = vt->rows - 1;
    vt->cur_col = col;
    vt->cur_row = row;
}

void vt_clear(vt_buf_t *vt)
{
    vt->cur_col = 0;
    vt->cur_row = 0;
    if (!vt->cells) return;
    size_t n = (size_t)vt->cols * (size_t)vt->rows;
    for (size_t i = 0; i < n; i++)
        vt_fill_cell(&vt->cells[i], ' ', vt->fg, vt->bg);
}

static void vt_scroll_up(vt_buf_t *vt)
{
    if (vt->rows <= 1) {
        vt_clear(vt);
        return;
    }
    size_t row_bytes = (size_t)vt->cols * sizeof(vt_cell_t);
    memmove(vt->cells, vt->cells + vt->cols, (size_t)(vt->rows - 1) * row_bytes);
    vt_cell_t *last = vt->cells + (size_t)(vt->rows - 1) * vt->cols;
    for (uint32_t c = 0; c < vt->cols; c++)
        vt_fill_cell(&last[c], ' ', vt->fg, vt->bg);
}

void vt_put_at(vt_buf_t *vt, char c, uint32_t col, uint32_t row)
{
    if (!vt->cells) return;
    if (col >= vt->cols || row >= vt->rows) return;
    vt_fill_cell(&vt->cells[(size_t)row * vt->cols + col],
                 (uint8_t)c, vt->fg, vt->bg);
}

void vt_putchar(vt_buf_t *vt, char c)
{
    if (!vt->cells || vt->cols == 0 || vt->rows == 0) return;

    /* Clamp transient out-of-range cursor (preemption between increment
     * and bounds check elsewhere). */
    if (vt->cur_col >= vt->cols) {
        vt->cur_col = 0;
        if (++vt->cur_row >= vt->rows) {
            vt_scroll_up(vt);
            vt->cur_row = vt->rows - 1;
        }
    }
    if (vt->cur_row >= vt->rows) {
        vt_scroll_up(vt);
        vt->cur_row = vt->rows - 1;
    }

    if (c == '\n') {
        vt->cur_col = 0;
        if (++vt->cur_row >= vt->rows) {
            vt_scroll_up(vt);
            vt->cur_row = vt->rows - 1;
        }
        return;
    }

    if (c == '\r') {
        vt->cur_col = 0;
        return;
    }

    if (c == '\b') {
        if (vt->cur_col > 0)
            vt->cur_col--;
        vt_fill_cell(&vt->cells[(size_t)vt->cur_row * vt->cols + vt->cur_col],
                     ' ', vt->fg, vt->bg);
        return;
    }

    vt_fill_cell(&vt->cells[(size_t)vt->cur_row * vt->cols + vt->cur_col],
                 (uint8_t)c, vt->fg, vt->bg);
    if (++vt->cur_col >= vt->cols) {
        vt->cur_col = 0;
        if (++vt->cur_row >= vt->rows) {
            vt_scroll_up(vt);
            vt->cur_row = vt->rows - 1;
        }
    }
}

vt_cell_t vt_get_cell(const vt_buf_t *vt, uint32_t col, uint32_t row)
{
    vt_cell_t zero = { 0, 0, 0, 0 };
    if (!vt || !vt->cells || col >= vt->cols || row >= vt->rows) return zero;
    return vt->cells[(size_t)row * vt->cols + col];
}
