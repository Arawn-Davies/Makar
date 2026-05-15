#ifndef _KERNEL_VT_H
#define _KERNEL_VT_H

/*
 * vt - virtual console backing grid.
 *
 * One vt_buf_t per virtual terminal.  Holds a cols x rows grid of cells
 * ({char, fg, bg}), the current cursor position, and the "current"
 * fg/bg attribute applied to incoming characters.  Pure data layer -
 * no framebuffer knowledge.
 *
 * Modelled on Linux's vc_data.vc_screenbuf and the ELKS / xv6 console
 * backing-buffer pattern.  vtty owns one buffer per TTY slot; the
 * vesa_tty / VGA renderers consult the current task's buffer when
 * deciding whether a write also needs to hit the physical framebuffer.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct vt_cell {
    uint8_t  ch;
    uint8_t  flags;   /* reserved for bold/inverse/etc. */
    uint32_t fg;      /* framebuffer-pixel-encoded foreground */
    uint32_t bg;      /* framebuffer-pixel-encoded background */
} vt_cell_t;

typedef struct vt_buf {
    uint32_t   cols;
    uint32_t   rows;
    uint32_t   cur_col;
    uint32_t   cur_row;
    uint32_t   fg;        /* current attribute applied to incoming chars */
    uint32_t   bg;
    vt_cell_t *cells;     /* row-major: cells[row * cols + col] */
} vt_buf_t;

/* Result of vt_putchar - lets the renderer know what to repaint without
 * having to diff before/after state itself. */
typedef struct vt_dirty {
    int      scrolled;   /* 1 if the grid scrolled; full repaint needed */
    int      has_cell;   /* 1 if a single cell was written (col,row valid) */
    uint32_t col;
    uint32_t row;
} vt_dirty_t;

/* Allocate the cell grid on the heap.  cols * rows cells, all set to
 * {' ', 0, fg, bg} so the buffer is paint-ready immediately.  Returns
 * true on success, false on allocation failure (caller may keep using
 * the buffer with cells == NULL; vt_putchar / vt_clear become no-ops). */
bool vt_init(vt_buf_t *vt, uint32_t cols, uint32_t rows,
             uint32_t default_fg, uint32_t default_bg);

/* Write c into the grid at the current cursor, advancing it.  Honours
 * \n, \r, \b.  Scrolls the grid up by one row when the cursor walks off
 * the bottom (top row is discarded).  No-op if cells == NULL.
 *
 * Returns a vt_dirty_t describing what changed so the renderer can
 * paint exactly the affected cells:
 *   - .scrolled == 1: the grid scrolled; renderer must repaint all rows.
 *   - .has_cell == 1: the single cell at (col, row) was modified.
 *   - both 0: control character with no visible effect (just cursor moved). */
vt_dirty_t vt_putchar(vt_buf_t *vt, char c);

/* Write c at (col, row) without moving the cursor.  Bounds-checked. */
void vt_put_at(vt_buf_t *vt, char c, uint32_t col, uint32_t row);

/* Set the current attribute applied to subsequent vt_putchar / vt_put_at
 * calls.  Does not repaint existing cells. */
void vt_set_color(vt_buf_t *vt, uint32_t fg, uint32_t bg);

/* Move the cursor.  Coordinates are clamped to the grid. */
void vt_set_cursor(vt_buf_t *vt, uint32_t col, uint32_t row);

/* Fill the grid with the current bg colour and reset cursor to (0,0). */
void vt_clear(vt_buf_t *vt);

/* Read-only accessor for cell (col, row).  Returns a zero-filled cell
 * for out-of-bounds reads (caller must not pass NULL vt). */
vt_cell_t vt_get_cell(const vt_buf_t *vt, uint32_t col, uint32_t row);

#endif /* _KERNEL_VT_H */
