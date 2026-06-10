/*
 * vt100.h -- a small ANSI/VT100+ terminal emulator core (parser + cell grid).
 *
 * Freestanding/userspace: no libc beyond memcpy/memset.  Feed it the byte
 * stream from a child program with vt_putc(); it maintains a colour cell grid
 * that a front-end (mxterm) renders.  Reusable by any terminal surface (a
 * future serial console can share it).
 *
 * Coverage: C0 controls; CSI cursor moves (CUU/CUD/CUF/CUB/CNL/CPL/CHA/VPA/
 * CUP/HVP), ED/EL erase, SGR colours (30-37/40-47 + bright 90-97/100-107 +
 * bold/reset/reverse), DECSTBM scroll region + SU/SD, IL/DL/ICH/DCH/ECH,
 * DECTCEM cursor show/hide, DECSET/RST 1049 alt-screen, ESC 7/8 save/restore,
 * ESC M reverse-index, ESC c reset.  OSC is consumed (title ignored).  Unknown
 * sequences are swallowed, never printed as garbage.  UTF-8 is decoded so a
 * multibyte codepoint occupies one cell (rendered as ASCII if <128, else '?').
 */
#ifndef VT100_H
#define VT100_H

#define VT_MAXCOLS  200
#define VT_MAXROWS  100
#define VT_MAXPARAM 16
#define VT_SCROLLBACK 500       /* history lines kept for the scrollbar         */

/* Colour indices 0-7 normal, 8-15 bright (bold brightens 0-7).  Defaults:
 * fg 7 (light grey), bg 0 (black).  A front-end maps these to RGB. */
typedef struct { unsigned char ch, fg, bg; } vt_cell;

typedef struct {
    int cols, rows;
    vt_cell cell[VT_MAXROWS][VT_MAXCOLS];

    int cx, cy;                 /* cursor (0-based) */
    int s_top, s_bot;           /* scroll region, 0-based inclusive */

    unsigned char cur_fg, cur_bg;
    int bold, reverse;

    int saved_cx, saved_cy;     /* ESC 7 / ESC 8 */
    int cursor_visible;

    /* alt-screen (?1049): saved primary grid + cursor */
    int alt_active;
    vt_cell alt_save[VT_MAXROWS][VT_MAXCOLS];
    int alt_cx, alt_cy;

    /* parser */
    int state;                  /* VT_GROUND / VT_ESC / VT_CSI / VT_OSC */
    int params[VT_MAXPARAM];
    int nparam, priv;

    /* UTF-8 decode */
    int u8_left;
    unsigned int u8_cp;

    /* scrollback ring: full-width lines that scrolled off the top of the primary
     * screen (alt-screen output is not captured).  History line i is at
     * sb[(sb_head - sb_count + i) mod VT_SCROLLBACK]. */
    vt_cell sb[VT_SCROLLBACK][VT_MAXCOLS];
    int sb_count, sb_head;
} vt_term;

void vt_init(vt_term *t, int cols, int rows);
void vt_resize(vt_term *t, int cols, int rows);
void vt_putc(vt_term *t, unsigned char b);

#endif /* VT100_H */
