/*
 * vt.c - virtual console backing grid.  See kernel/vt.h.
 */

#include <kernel/vt.h>
#include <kernel/heap.h>
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
    vt->usable_rows = rows;
    vt->cur_col = 0;
    vt->cur_row = 0;
    vt->fg      = default_fg;
    vt->bg      = default_bg;

    vt->esc_state  = 0;
    vt->esc_nparam = 0;
    vt->esc_priv   = 0;
    for (uint32_t i = 0; i < VT_NPARAM; i++) vt->esc_param[i] = 0;
    vt->saved_col = 0;
    vt->saved_row = 0;
    vt->def_fg    = default_fg;
    vt->def_bg    = default_bg;

    size_t n = (size_t)cols * (size_t)rows;
    vt->cells = (vt_cell_t *)kmalloc(n * sizeof(vt_cell_t));
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

static uint32_t vt_limit_rows(const vt_buf_t *vt)
{
    if (!vt || vt->rows == 0) return 0;
    if (vt->usable_rows == 0 || vt->usable_rows > vt->rows)
        return vt->rows;
    return vt->usable_rows;
}

void vt_set_usable_rows(vt_buf_t *vt, uint32_t rows)
{
    if (!vt || vt->rows == 0) return;
    if (rows == 0 || rows > vt->rows)
        rows = vt->rows;
    vt->usable_rows = rows;
    if (vt->cur_row >= rows)
        vt->cur_row = rows - 1;
    if (vt->cur_col >= vt->cols && vt->cols > 0)
        vt->cur_col = vt->cols - 1;
}

void vt_set_cursor(vt_buf_t *vt, uint32_t col, uint32_t row)
{
    uint32_t rows = vt_limit_rows(vt);
    if (vt->cols == 0 || rows == 0) return;
    if (col >= vt->cols) col = vt->cols - 1;
    if (row >= rows) row = rows - 1;
    vt->cur_col = col;
    vt->cur_row = row;
}

void vt_clear(vt_buf_t *vt)
{
    vt->cur_col = 0;
    vt->cur_row = 0;
    if (!vt->cells) return;
    size_t n = (size_t)vt->cols * (size_t)vt_limit_rows(vt);
    for (size_t i = 0; i < n; i++)
        vt_fill_cell(&vt->cells[i], ' ', vt->fg, vt->bg);
}

static void vt_scroll_up(vt_buf_t *vt)
{
    uint32_t rows = vt_limit_rows(vt);
    if (rows <= 1) {
        vt_clear(vt);
        return;
    }
    size_t row_bytes = (size_t)vt->cols * sizeof(vt_cell_t);
    memmove(vt->cells, vt->cells + vt->cols, (size_t)(rows - 1) * row_bytes);
    vt_cell_t *last = vt->cells + (size_t)(rows - 1) * vt->cols;
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

/* Standard 16-colour ANSI palette, framebuffer-pixel encoded (0x00RRGGBB).
 * Index order is ANSI (0 black .. 7 white, 8-15 bright), matching the SGR
 * codes 30-37/90-97 and the mxterm client palette. */
static const uint32_t s_ansi_pal[16] = {
    0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500,
    0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,
    0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
    0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

/* Fill an inclusive cell region [r0,c0]..[r1,c1] with the current bg (used by
 * ED / EL).  Clamped to the usable area. */
static void vt_fill_region(vt_buf_t *vt, uint32_t r0, uint32_t c0,
                           uint32_t r1, uint32_t c1)
{
    uint32_t rows = vt_limit_rows(vt);
    if (!vt->cells) return;
    for (uint32_t r = r0; r <= r1 && r < rows; r++) {
        uint32_t cs = (r == r0) ? c0 : 0;
        uint32_t ce = (r == r1) ? c1 : (vt->cols - 1);
        for (uint32_t c = cs; c <= ce && c < vt->cols; c++)
            vt_fill_cell(&vt->cells[(size_t)r * vt->cols + c], ' ', vt->fg, vt->bg);
    }
}

/* CSI param i, treating 0/absent as the given default. */
static int vt_pget(const vt_buf_t *vt, int i, int def)
{
    int v = (i < vt->esc_nparam) ? vt->esc_param[i] : 0;
    return v ? v : def;
}

/* SGR (Select Graphic Rendition): map colour codes to grid fg/bg.  Bold and
 * reverse aren't tracked as flags here -- bright colours arrive as their own
 * 90-97/100-107 codes (which is exactly what the cell-API->ANSI bridge and
 * mxterm emit), so the rendered result is identical. */
static void vt_sgr(vt_buf_t *vt)
{
    if (vt->esc_nparam == 0) { vt->fg = vt->def_fg; vt->bg = vt->def_bg; return; }
    for (int i = 0; i < vt->esc_nparam; i++) {
        int p = vt->esc_param[i];
        if      (p == 0)                 { vt->fg = vt->def_fg; vt->bg = vt->def_bg; }
        else if (p >= 30 && p <= 37)     vt->fg = s_ansi_pal[p - 30];
        else if (p == 39)                vt->fg = vt->def_fg;
        else if (p >= 40 && p <= 47)     vt->bg = s_ansi_pal[p - 40];
        else if (p == 49)                vt->bg = vt->def_bg;
        else if (p >= 90 && p <= 97)     vt->fg = s_ansi_pal[p - 90 + 8];
        else if (p >= 100 && p <= 107)   vt->bg = s_ansi_pal[p - 100 + 8];
    }
}

/* Dispatch a completed CSI sequence (final byte f).  Returns 1 if grid
 * *content* changed (ED/EL), so the caller forces a full repaint; pure cursor
 * moves return 0. */
static int vt_csi_dispatch(vt_buf_t *vt, unsigned char f)
{
    uint32_t rows = vt_limit_rows(vt);
    int n;
    switch (f) {
    case 'A': n = vt_pget(vt,0,1); vt->cur_row = (vt->cur_row > (uint32_t)n) ? vt->cur_row - n : 0; break;
    case 'B': n = vt_pget(vt,0,1); vt->cur_row += n; if (vt->cur_row >= rows) vt->cur_row = rows - 1; break;
    case 'C': n = vt_pget(vt,0,1); vt->cur_col += n; if (vt->cur_col >= vt->cols) vt->cur_col = vt->cols - 1; break;
    case 'D': n = vt_pget(vt,0,1); vt->cur_col = (vt->cur_col > (uint32_t)n) ? vt->cur_col - n : 0; break;
    case 'E': n = vt_pget(vt,0,1); vt->cur_row += n; if (vt->cur_row >= rows) vt->cur_row = rows - 1; vt->cur_col = 0; break;
    case 'F': n = vt_pget(vt,0,1); vt->cur_row = (vt->cur_row > (uint32_t)n) ? vt->cur_row - n : 0; vt->cur_col = 0; break;
    case 'G': case '`': { int c = vt_pget(vt,0,1) - 1; if (c < 0) c = 0; vt->cur_col = (uint32_t)c;
                          if (vt->cur_col >= vt->cols) vt->cur_col = vt->cols - 1; break; }
    case 'd': { int r = vt_pget(vt,0,1) - 1; if (r < 0) r = 0; vt->cur_row = (uint32_t)r;
                if (vt->cur_row >= rows) vt->cur_row = rows - 1; break; }
    case 'H': case 'f': {
        int r = vt_pget(vt,0,1) - 1, c = vt_pget(vt,1,1) - 1;
        if (r < 0) r = 0; if (c < 0) c = 0;
        vt->cur_row = (uint32_t)r; vt->cur_col = (uint32_t)c;
        if (vt->cur_row >= rows) vt->cur_row = rows - 1;
        if (vt->cur_col >= vt->cols) vt->cur_col = vt->cols - 1;
        break; }
    case 'J': {  /* ED: erase in display */
        int m = (vt->esc_nparam > 0) ? vt->esc_param[0] : 0;
        if      (m == 0) vt_fill_region(vt, vt->cur_row, vt->cur_col, rows - 1, vt->cols - 1);
        else if (m == 1) vt_fill_region(vt, 0, 0, vt->cur_row, vt->cur_col);
        else             vt_fill_region(vt, 0, 0, rows - 1, vt->cols - 1);
        return 1; }
    case 'K': {  /* EL: erase in line */
        int m = (vt->esc_nparam > 0) ? vt->esc_param[0] : 0;
        if      (m == 0) vt_fill_region(vt, vt->cur_row, vt->cur_col, vt->cur_row, vt->cols - 1);
        else if (m == 1) vt_fill_region(vt, vt->cur_row, 0, vt->cur_row, vt->cur_col);
        else             vt_fill_region(vt, vt->cur_row, 0, vt->cur_row, vt->cols - 1);
        return 1; }
    case 'm': vt_sgr(vt); break;
    case 's': vt->saved_col = vt->cur_col; vt->saved_row = vt->cur_row; break;
    case 'u': vt->cur_col = vt->saved_col; vt->cur_row = vt->saved_row;
              if (vt->cur_row >= rows) vt->cur_row = rows - 1;
              if (vt->cur_col >= vt->cols) vt->cur_col = vt->cols - 1; break;
    default: break;   /* swallow unimplemented finals */
    }
    return 0;
}

vt_dirty_t vt_putchar(vt_buf_t *vt, char c)
{
    vt_dirty_t d = { 0, 0, 0, 0 };
    uint32_t rows = vt_limit_rows(vt);
    if (!vt->cells || vt->cols == 0 || rows == 0) return d;

    /* ---- ANSI/VT100 escape-sequence state machine ----------------------
     * Non-ground states consume the byte and return; GROUND falls through to
     * the plain \n/\r/\b + glyph path that follows (unchanged). */
    unsigned char b = (unsigned char)c;
    if (vt->esc_state == 1) {               /* after ESC */
        switch (b) {
        case '[': vt->esc_state = 2; vt->esc_nparam = 0; vt->esc_priv = 0;
                  for (uint32_t i = 0; i < VT_NPARAM; i++) vt->esc_param[i] = 0; return d;
        case ']': vt->esc_state = 3; return d;                       /* OSC */
        case '7': vt->saved_col = vt->cur_col; vt->saved_row = vt->cur_row; vt->esc_state = 0; return d;
        case '8': vt->cur_col = vt->saved_col; vt->cur_row = vt->saved_row;
                  if (vt->cur_row >= rows) vt->cur_row = rows - 1;
                  if (vt->cur_col >= vt->cols) vt->cur_col = vt->cols - 1;
                  vt->esc_state = 0; return d;
        case 'c': vt->fg = vt->def_fg; vt->bg = vt->def_bg;          /* RIS */
                  vt_clear(vt); vt->esc_state = 0; d.scrolled = 1; return d;
        case '(': case ')': case '*': case '+':                      /* charset: eat next */
                  vt->esc_state = 4; return d;
        default:  vt->esc_state = 0; return d;
        }
    }
    if (vt->esc_state == 4) { vt->esc_state = 0; return d; }         /* swallowed charset byte */
    if (vt->esc_state == 2) {                /* CSI */
        if (b == '?') { vt->esc_priv = 1; return d; }
        if (b >= '0' && b <= '9') {
            int i = vt->esc_nparam ? vt->esc_nparam - 1 : 0;
            vt->esc_param[i] = (uint16_t)(vt->esc_param[i] * 10 + (b - '0'));
            if (!vt->esc_nparam) vt->esc_nparam = 1;
            return d;
        }
        if (b == ';') {
            if (!vt->esc_nparam) vt->esc_nparam = 1;
            if (vt->esc_nparam < VT_NPARAM) vt->esc_nparam++;
            return d;
        }
        if (b >= 0x40 && b <= 0x7E) {        /* final byte: dispatch */
            if (vt_csi_dispatch(vt, b)) d.scrolled = 1;
            vt->esc_state = 0;
            return d;
        }
        return d;                            /* intermediates: ignore */
    }
    if (vt->esc_state == 3) {                /* OSC: swallow to BEL or ESC \ */
        if (b == 0x07) vt->esc_state = 0;
        else if (b == 0x1B) vt->esc_state = 5;
        return d;
    }
    if (vt->esc_state == 5) { vt->esc_state = 0; return d; }         /* ST / abort */

    /* GROUND: ESC starts a sequence; everything else is text/control. */
    if (b == 0x1B) { vt->esc_state = 1; return d; }

    /* Clamp transient out-of-range cursor (preemption between increment
     * and bounds check elsewhere). */
    if (vt->cur_col >= vt->cols) {
        vt->cur_col = 0;
        if (++vt->cur_row >= rows) {
            vt_scroll_up(vt);
            vt->cur_row = rows - 1;
            d.scrolled = 1;
        }
    }
    if (vt->cur_row >= rows) {
        vt_scroll_up(vt);
        vt->cur_row = rows - 1;
        d.scrolled = 1;
    }

    if (c == '\n') {
        vt->cur_col = 0;
        if (++vt->cur_row >= rows) {
            vt_scroll_up(vt);
            vt->cur_row = rows - 1;
            d.scrolled = 1;
        }
        return d;
    }

    if (c == '\r') {
        vt->cur_col = 0;
        return d;
    }

    if (c == '\b') {
        if (vt->cur_col > 0)
            vt->cur_col--;
        vt_fill_cell(&vt->cells[(size_t)vt->cur_row * vt->cols + vt->cur_col],
                     ' ', vt->fg, vt->bg);
        d.has_cell = 1;
        d.col = vt->cur_col;
        d.row = vt->cur_row;
        return d;
    }

    uint32_t wrote_col = vt->cur_col;
    uint32_t wrote_row = vt->cur_row;
    vt_fill_cell(&vt->cells[(size_t)wrote_row * vt->cols + wrote_col],
                 (uint8_t)c, vt->fg, vt->bg);
    d.has_cell = 1;
    d.col = wrote_col;
    d.row = wrote_row;

    if (++vt->cur_col >= vt->cols) {
        vt->cur_col = 0;
        if (++vt->cur_row >= rows) {
            vt_scroll_up(vt);
            vt->cur_row = rows - 1;
            d.scrolled = 1;
        }
    }
    return d;
}

vt_cell_t vt_get_cell(const vt_buf_t *vt, uint32_t col, uint32_t row)
{
    vt_cell_t zero = { 0, 0, 0, 0 };
    if (!vt || !vt->cells || col >= vt->cols || row >= vt->rows) return zero;
    return vt->cells[(size_t)row * vt->cols + col];
}
