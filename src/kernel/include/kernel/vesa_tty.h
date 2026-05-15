#ifndef _KERNEL_VESA_TTY_H
#define _KERNEL_VESA_TTY_H

#include <stdbool.h>
#include <stdint.h>

struct vt_buf;

/*
 * vesa_tty - text renderer over the VESA framebuffer.
 *
 * Phase 1 of the split-pane work: rendering is now expressed against a
 * `vesa_pane_t` that owns its own cursor, colours, and a sub-rectangle of
 * the screen (full-width, top_row..top_row+rows-1).  The legacy global API
 * (vesa_tty_putchar etc.) delegates to a screen-spanning default pane so
 * existing callers are unaffected.
 *
 * Future phases will allow the shell, VICS, and a tmux-style splitter to
 * each hold their own pane.  Side-by-side (column-split) panes are out of
 * scope; only horizontal stacking (top/bottom) is supported.
 */

/* Opaque-ish pane handle.  Fields are public so callers can stack-allocate
 * panes without a heap dependency, but they should only be touched via the
 * vesa_tty_pane_* API. */
typedef struct vesa_pane {
	uint32_t top_row;   /* first cell row owned by this pane */
	uint32_t cols;      /* width in character cells */
	uint32_t rows;      /* height in character cells */
	uint32_t cur_col;   /* cursor column, relative to the pane (0..cols-1) */
	uint32_t cur_row;   /* cursor row,    relative to the pane (0..rows-1) */
	uint32_t fg;        /* foreground pixel (already composed for the FB) */
	uint32_t bg;        /* background pixel (already composed for the FB) */
} vesa_pane_t;

/* ------------------------------------------------------------------ */
/* Initialisation / status                                             */
/* ------------------------------------------------------------------ */

/* Initialise the renderer.  Must be called after vesa_init().  Sets up the
 * default pane covering the whole screen, clears the framebuffer, sets
 * default colours (white on black).  Returns false if no framebuffer is
 * available. */
bool vesa_tty_init(void);

/* True once vesa_tty_init() has succeeded. */
bool vesa_tty_is_ready(void);

/* Disable the renderer (marks not-ready).  Call before switching to VGA. */
void vesa_tty_disable(void);

/* Total character columns / rows on the physical screen. */
uint32_t vesa_tty_get_cols(void);
uint32_t vesa_tty_get_rows(void);

/* ------------------------------------------------------------------ */
/* Default pane - covers the whole screen                              */
/* ------------------------------------------------------------------ */

/* Returns the screen-spanning default pane (valid after vesa_tty_init).
 * Phase 2/3 may share this with non-split callers; do not free. */
vesa_pane_t *vesa_tty_default_pane(void);

/* ------------------------------------------------------------------ */
/* Pane API                                                            */
/* ------------------------------------------------------------------ */

/* Initialise a caller-allocated pane covering the full screen width and
 * the row range [top_row, top_row+rows).  Cursor goes to (0,0).  Colours
 * are reset to white-on-black; tweak with vesa_tty_pane_setcolor().
 * Safe to call before vesa_tty_init() - it just zero-fills the struct. */
void vesa_tty_pane_init(vesa_pane_t *p, uint32_t top_row, uint32_t rows);

/* Set fg/bg as 24-bit RGB (0x00RRGGBB).  Stored in framebuffer-pixel form. */
void vesa_tty_pane_setcolor(vesa_pane_t *p, uint32_t fg_rgb, uint32_t bg_rgb);

/* Render a character at the pane's cursor, advancing it.  Honours \n, \r,
 * \b.  Scrolls within the pane only.  No-op if vesa_tty is not ready. */
void vesa_tty_pane_putchar(vesa_pane_t *p, char c);

/* Render a character at pane-relative cell (col, row) without moving the
 * cursor.  Out-of-pane writes are clipped. */
void vesa_tty_pane_put_at(vesa_pane_t *p, char c, uint32_t col, uint32_t row);

/* Move the cursor (pane-relative coordinates). */
void vesa_tty_pane_set_cursor(vesa_pane_t *p, uint32_t col, uint32_t row);

/* Fill the pane with its current background and reset its cursor to (0,0). */
void vesa_tty_pane_clear(vesa_pane_t *p);

/* Pane-relative cursor accessors. */
uint32_t vesa_tty_pane_get_col(const vesa_pane_t *p);
uint32_t vesa_tty_pane_get_row(const vesa_pane_t *p);

/* ------------------------------------------------------------------ */
/* Legacy global API (delegates to the default pane)                   */
/* ------------------------------------------------------------------ */

uint32_t vesa_tty_get_col(void);
uint32_t vesa_tty_get_row(void);
void vesa_tty_putchar(char c);
void vesa_tty_setcolor(uint32_t fg_rgb, uint32_t bg_rgb);
void vesa_tty_put_at(char c, uint32_t col, uint32_t row);
void vesa_tty_set_cursor(uint32_t col, uint32_t row);
void vesa_tty_clear(void);

/* Animate a spinning cursor at the top-right corner of the screen.
 * Always renders into the default pane regardless of focus. */
void vesa_tty_spinner_tick(uint32_t tick);

/* ------------------------------------------------------------------ */
/* Backing-grid integration                                            */
/* ------------------------------------------------------------------ */

/* Paint a single cell at (col, row) using explicit framebuffer-pixel
 * fg/bg.  Bypasses cursor / scrolling logic; used by vtty_switch when
 * replaying a VT's backing grid. */
void vesa_tty_paint_cell(uint32_t col, uint32_t row, char ch,
                         uint32_t fg, uint32_t bg);

/* Repaint the entire framebuffer from a backing vt_buf.  Used by
 * vtty_switch to surface a background TTY's accumulated state. */
void vesa_tty_paint_buf(const struct vt_buf *vt);

/* Paint a string starting at cell (col, row) using explicit RGB colours.
 * Bypasses cursor / scrolling / vt_buf routing - direct FB write.  Used
 * for status-bar overlay rows that live outside any TTY's grid. */
void vesa_tty_paint_string_at(uint32_t col, uint32_t row,
                              const char *s,
                              uint32_t fg_rgb, uint32_t bg_rgb);

/* Paint the tmux-style status bar at the bottom row of the screen.
 * Active TTY is highlighted with inverse video.  active=current VT,
 * count=number of registered slots.  No-op if vesa_tty isn't ready. */
void vesa_tty_paint_status(int active, int count);

/* ------------------------------------------------------------------ */
/* Visible caret on the default pane                                   */
/* ------------------------------------------------------------------ */

#define VESA_CARET_UNDERSCORE  0u
#define VESA_CARET_BLOCK       1u
#define VESA_CARET_FLASH       2u

void     vesa_tty_set_caret_style(uint32_t style);
uint32_t vesa_tty_get_caret_style(void);

/* Drive caret blink animation.  Pass the current kernel tick count.  No-op
 * unless the FLASH style is active.  Safe to call frequently from the
 * keyboard wait loop. */
void vesa_tty_caret_blink_tick(uint32_t ticks);

/* Change the font scale factor (1 = small, 2 = large).  Clears the screen
 * and re-initialises the default pane to the new geometry. */
void vesa_tty_set_scale(uint32_t scale);

#endif
