/* vt100.c -- ANSI/VT100+ terminal emulator core.  See vt100.h. */
#include "vt100.h"

/* Local mem helpers so the emulator stays freestanding (mxterm links without
 * libc.a, like the other makx clients). */
static void *memset(void *d, int v, unsigned long n)
{ unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
static void *memcpy(void *d, const void *s, unsigned long n)
{ unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }

enum { VT_GROUND=0, VT_ESC, VT_CSI, VT_OSC };

/* ---- cell helpers -------------------------------------------------------- */

/* Final fg/bg for a write, applying bold (brightens 0-7) then reverse-video. */
static void cur_colours(vt_term *t, unsigned char *fg, unsigned char *bg)
{
    unsigned char f = t->cur_fg, b = t->cur_bg;
    if (t->bold && f < 8) f += 8;
    if (t->reverse) { unsigned char x = f; f = b; b = x; }
    *fg = f; *bg = b;
}
static vt_cell blank_cell(vt_term *t)
{
    vt_cell c; unsigned char f, b; cur_colours(t, &f, &b);
    c.ch = ' '; c.fg = f; c.bg = b; return c;
}
static void clear_region(vt_term *t, int r0, int c0, int r1, int c1)
{
    vt_cell bl = blank_cell(t);
    for (int r = r0; r <= r1 && r < t->rows; r++)
        for (int c = (r==r0?c0:0); c <= (r==r1?c1:t->cols-1) && c < t->cols; c++)
            t->cell[r][c] = bl;
}

/* ---- scrolling within the current region --------------------------------- */
/* Copy a primary-screen row into the scrollback ring before it is overwritten. */
static void sb_push(vt_term *t, int row)
{
    if (t->alt_active) return;                       /* alt-screen: no history */
    vt_cell bl = blank_cell(t);
    vt_cell *dst = t->sb[t->sb_head];
    for (int c = 0; c < t->cols; c++)          dst[c] = t->cell[row][c];
    for (int c = t->cols; c < VT_MAXCOLS; c++)  dst[c] = bl;
    t->sb_head = (t->sb_head + 1) % VT_SCROLLBACK;
    if (t->sb_count < VT_SCROLLBACK) t->sb_count++;
}
static void scroll_up(vt_term *t, int n)
{
    if (n < 1) return;
    /* A full-screen scroll evicts the top n rows -> save them to scrollback. */
    if (t->s_top == 0 && t->s_bot == t->rows - 1) {
        int k = n; if (k > t->rows) k = t->rows;
        for (int i = 0; i < k; i++) sb_push(t, i);
    }
    vt_cell bl = blank_cell(t);
    for (int r = t->s_top; r <= t->s_bot; r++) {
        int src = r + n;
        for (int c = 0; c < t->cols; c++)
            t->cell[r][c] = (src <= t->s_bot) ? t->cell[src][c] : bl;
    }
}
static void scroll_down(vt_term *t, int n)
{
    if (n < 1) return;
    vt_cell bl = blank_cell(t);
    for (int r = t->s_bot; r >= t->s_top; r--) {
        int src = r - n;
        for (int c = 0; c < t->cols; c++)
            t->cell[r][c] = (src >= t->s_top) ? t->cell[src][c] : bl;
    }
}

/* ---- cursor / output ----------------------------------------------------- */
static void clampc(vt_term *t)
{
    if (t->cx < 0) t->cx = 0; if (t->cx >= t->cols) t->cx = t->cols-1;
    if (t->cy < 0) t->cy = 0; if (t->cy >= t->rows) t->cy = t->rows-1;
}
static void line_feed(vt_term *t)
{
    if (t->cy == t->s_bot) scroll_up(t, 1);
    else if (t->cy < t->rows-1) t->cy++;
}
static void put_glyph(vt_term *t, unsigned int cp)
{
    if (t->cx >= t->cols) { t->cx = 0; line_feed(t); }
    unsigned char f, b; cur_colours(t, &f, &b);
    vt_cell *c = &t->cell[t->cy][t->cx];
    c->ch = (cp < 128u) ? (unsigned char)cp : '?';
    c->fg = f; c->bg = b;
    t->cx++;
}

/* ---- init / resize ------------------------------------------------------- */
void vt_init(vt_term *t, int cols, int rows)
{
    memset(t, 0, sizeof(*t));
    if (cols < 1) cols = 1; if (cols > VT_MAXCOLS) cols = VT_MAXCOLS;
    if (rows < 1) rows = 1; if (rows > VT_MAXROWS) rows = VT_MAXROWS;
    t->cols = cols; t->rows = rows;
    t->cur_fg = 7; t->cur_bg = 0;
    t->s_top = 0; t->s_bot = rows-1;
    t->cursor_visible = 1;
    vt_cell bl = blank_cell(t);
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) t->cell[r][c] = bl;
}
void vt_resize(vt_term *t, int cols, int rows)
{
    if (cols < 1) cols = 1; if (cols > VT_MAXCOLS) cols = VT_MAXCOLS;
    if (rows < 1) rows = 1; if (rows > VT_MAXROWS) rows = VT_MAXROWS;
    /* Blank any newly-exposed cells; keep existing content in place. */
    vt_cell bl = blank_cell(t);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (r >= t->rows || c >= t->cols) t->cell[r][c] = bl;
    t->cols = cols; t->rows = rows;
    t->s_top = 0; t->s_bot = rows-1;        /* reset scroll region on resize */
    clampc(t);
}

/* ---- CSI dispatch -------------------------------------------------------- */
static int pget(vt_term *t, int i, int def)       /* param i, 0/absent -> def */
{
    int v = (i < t->nparam) ? t->params[i] : 0;
    return v ? v : def;
}
static void csi_dispatch(vt_term *t, unsigned char f)
{
    int n;
    switch (f) {
    case 'A': t->cy -= pget(t,0,1); if (t->cy < t->s_top) t->cy = t->s_top; break;
    case 'B': t->cy += pget(t,0,1); if (t->cy > t->s_bot) t->cy = t->s_bot; break;
    case 'C': t->cx += pget(t,0,1); if (t->cx > t->cols-1) t->cx = t->cols-1; break;
    case 'D': t->cx -= pget(t,0,1); if (t->cx < 0) t->cx = 0; break;
    case 'E': t->cy += pget(t,0,1); t->cx = 0; clampc(t); break;
    case 'F': t->cy -= pget(t,0,1); t->cx = 0; clampc(t); break;
    case 'G': case '`': t->cx = pget(t,0,1)-1; clampc(t); break;
    case 'd': t->cy = pget(t,0,1)-1; clampc(t); break;
    case 'H': case 'f':
        t->cy = pget(t,0,1)-1; t->cx = pget(t,1,1)-1; clampc(t); break;
    case 'J': {                                   /* ED */
        int m = (0 < t->nparam) ? t->params[0] : 0;
        if (m == 0)      clear_region(t, t->cy, t->cx, t->rows-1, t->cols-1);
        else if (m == 1) clear_region(t, 0, 0, t->cy, t->cx);
        else             clear_region(t, 0, 0, t->rows-1, t->cols-1);
        break; }
    case 'K': {                                   /* EL */
        int m = (0 < t->nparam) ? t->params[0] : 0;
        if (m == 0)      clear_region(t, t->cy, t->cx, t->cy, t->cols-1);
        else if (m == 1) clear_region(t, t->cy, 0, t->cy, t->cx);
        else             clear_region(t, t->cy, 0, t->cy, t->cols-1);
        break; }
    case 'S': scroll_up(t, pget(t,0,1)); break;
    case 'T': scroll_down(t, pget(t,0,1)); break;
    case 'L': {                                   /* IL: insert lines at cy */
        if (t->cy < t->s_top || t->cy > t->s_bot) break;
        n = pget(t,0,1);
        for (int i = 0; i < n; i++) {
            for (int r = t->s_bot; r > t->cy; r--)
                memcpy(t->cell[r], t->cell[r-1], sizeof(vt_cell)*t->cols);
            vt_cell bl = blank_cell(t);
            for (int c = 0; c < t->cols; c++) t->cell[t->cy][c] = bl;
        }
        break; }
    case 'M': {                                   /* DL: delete lines at cy */
        if (t->cy < t->s_top || t->cy > t->s_bot) break;
        n = pget(t,0,1);
        for (int i = 0; i < n; i++) {
            for (int r = t->cy; r < t->s_bot; r++)
                memcpy(t->cell[r], t->cell[r+1], sizeof(vt_cell)*t->cols);
            vt_cell bl = blank_cell(t);
            for (int c = 0; c < t->cols; c++) t->cell[t->s_bot][c] = bl;
        }
        break; }
    case '@': {                                   /* ICH: insert blanks */
        n = pget(t,0,1); vt_cell bl = blank_cell(t);
        for (int c = t->cols-1; c >= t->cx; c--)
            t->cell[t->cy][c] = (c-n >= t->cx) ? t->cell[t->cy][c-n] : bl;
        break; }
    case 'P': {                                   /* DCH: delete chars */
        n = pget(t,0,1); vt_cell bl = blank_cell(t);
        for (int c = t->cx; c < t->cols; c++)
            t->cell[t->cy][c] = (c+n < t->cols) ? t->cell[t->cy][c+n] : bl;
        break; }
    case 'X': {                                   /* ECH: erase chars */
        n = pget(t,0,1);
        clear_region(t, t->cy, t->cx, t->cy, t->cx+n-1 < t->cols ? t->cx+n-1 : t->cols-1);
        break; }
    case 'r':                                     /* DECSTBM scroll region */
        t->s_top = pget(t,0,1)-1;
        t->s_bot = pget(t,1,t->rows)-1;
        if (t->s_top < 0) t->s_top = 0;
        if (t->s_bot > t->rows-1) t->s_bot = t->rows-1;
        if (t->s_top >= t->s_bot) { t->s_top = 0; t->s_bot = t->rows-1; }
        t->cx = 0; t->cy = t->s_top;
        break;
    case 'h': case 'l': {                          /* DECSET/RST (private) */
        int set = (f == 'h');
        if (t->priv) {
            int m = (0 < t->nparam) ? t->params[0] : 0;
            if (m == 25) t->cursor_visible = set;
            else if (m == 1049) {                  /* alt-screen */
                if (set && !t->alt_active) {
                    memcpy(t->alt_save, t->cell, sizeof(t->cell));
                    t->alt_cx = t->cx; t->alt_cy = t->cy;
                    t->alt_active = 1;
                    clear_region(t, 0, 0, t->rows-1, t->cols-1);
                    t->cx = t->cy = 0;
                } else if (!set && t->alt_active) {
                    memcpy(t->cell, t->alt_save, sizeof(t->cell));
                    t->cx = t->alt_cx; t->cy = t->alt_cy;
                    t->alt_active = 0;
                }
            }
        }
        break; }
    case 'm': {                                    /* SGR */
        if (t->nparam == 0) { t->cur_fg = 7; t->cur_bg = 0; t->bold = t->reverse = 0; break; }
        for (int i = 0; i < t->nparam; i++) {
            int p = t->params[i];
            if (p == 0)              { t->cur_fg = 7; t->cur_bg = 0; t->bold = t->reverse = 0; }
            else if (p == 1)         t->bold = 1;
            else if (p == 22)        t->bold = 0;
            else if (p == 7)         t->reverse = 1;
            else if (p == 27)        t->reverse = 0;
            else if (p >= 30 && p <= 37)   t->cur_fg = (unsigned char)(p-30);
            else if (p == 39)        t->cur_fg = 7;
            else if (p >= 40 && p <= 47)   t->cur_bg = (unsigned char)(p-40);
            else if (p == 49)        t->cur_bg = 0;
            else if (p >= 90 && p <= 97)   t->cur_fg = (unsigned char)(p-90+8);
            else if (p >= 100 && p <= 107) t->cur_bg = (unsigned char)(p-100+8);
        }
        break; }
    default: break;                                /* swallow unknown */
    }
}

/* ---- byte feed ----------------------------------------------------------- */
void vt_putc(vt_term *t, unsigned char b)
{
    switch (t->state) {
    case VT_ESC:
        switch (b) {
        case '[': t->state = VT_CSI; t->nparam = 0; t->priv = 0;
                  for (int i = 0; i < VT_MAXPARAM; i++) t->params[i] = 0; return;
        case ']': t->state = VT_OSC; return;
        case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; t->state = VT_GROUND; return;
        case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; clampc(t); t->state = VT_GROUND; return;
        case 'M': if (t->cy == t->s_top) scroll_down(t,1); else if (t->cy>0) t->cy--;
                  t->state = VT_GROUND; return;
        case 'c': vt_init(t, t->cols, t->rows); return;        /* RIS */
        case '(': case ')': case '*': case '+':                /* charset: eat next */
                  t->state = VT_ESC + 100; return;             /* one-byte swallow */
        default:  t->state = VT_GROUND; return;
        }
    case VT_ESC + 100: t->state = VT_GROUND; return;           /* swallowed charset byte */
    case VT_CSI:
        if (b == '?') { t->priv = 1; return; }
        if (b >= '0' && b <= '9') {
            int i = t->nparam ? t->nparam-1 : 0;
            t->params[i] = t->params[i]*10 + (b-'0');
            if (!t->nparam) t->nparam = 1;
            return;
        }
        if (b == ';') {
            if (!t->nparam) t->nparam = 1;
            if (t->nparam < VT_MAXPARAM) t->nparam++;
            return;
        }
        if (b >= 0x40 && b <= 0x7E) { csi_dispatch(t, b); t->state = VT_GROUND; return; }
        return;                                                /* intermediates: ignore */
    case VT_OSC:
        if (b == 0x07) t->state = VT_GROUND;                   /* BEL terminates */
        else if (b == 0x1B) t->state = VT_ESC + 200;           /* maybe ST (ESC \) */
        return;
    case VT_ESC + 200:
        t->state = VT_GROUND; return;                          /* ST or abort OSC */
    default: break;                                            /* VT_GROUND below */
    }

    /* GROUND */
    if (b == 0x1B) { t->state = VT_ESC; return; }
    if (b < 0x20) {                                            /* C0 controls */
        switch (b) {
        /* Makar convention (matches the kernel VGA console, t_putchar): a bare
         * '\n' is a *newline* -- carriage-return + line-feed -- since apps emit
         * '\n' between lines rather than "\r\n".  (A strict VT would move down
         * only; that left prompts cascading diagonally.) */
        case '\n': t->cx = 0; line_feed(t); break;
        case '\r': t->cx = 0; break;
        case '\b': if (t->cx > 0) t->cx--; break;
        case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx > t->cols-1) t->cx = t->cols-1; break;
        case 0x07: break;                                      /* BEL: no-op */
        default: break;
        }
        return;
    }

    /* UTF-8 decode -> one cell per codepoint (ASCII renders, else '?'). */
    if (b < 0x80) { t->u8_left = 0; put_glyph(t, b); return; }
    if ((b & 0xC0) == 0x80) {                                  /* continuation */
        if (t->u8_left) { t->u8_cp = (t->u8_cp << 6) | (b & 0x3F);
                          if (--t->u8_left == 0) put_glyph(t, t->u8_cp); }
        return;
    }
    if ((b & 0xE0) == 0xC0) { t->u8_cp = b & 0x1F; t->u8_left = 1; return; }
    if ((b & 0xF0) == 0xE0) { t->u8_cp = b & 0x0F; t->u8_left = 2; return; }
    if ((b & 0xF8) == 0xF0) { t->u8_cp = b & 0x07; t->u8_left = 3; return; }
    t->u8_left = 0;                                            /* invalid lead */
}
