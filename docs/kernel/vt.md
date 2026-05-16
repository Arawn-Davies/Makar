---
title: vt
parent: Kernel subsystems
---

# vt - virtual console backing grid

`src/kernel/include/kernel/vt.h` + `src/kernel/arch/i386/display/vt.c`.

## Purpose

`vt_buf_t` is a logical `cols × rows` character grid plus cursor and current
attribute. One instance per virtual terminal (allocated and owned by
[vtty](vtty.md)). Pure data layer - no framebuffer knowledge.

Modelled on Linux's `vc_data.vc_screenbuf` and the ELKS / xv6 console
backing-buffer pattern. Lets background TTYs accumulate output silently and
have it surface atomically when the user switches focus.

## Data layout

```c
typedef struct vt_cell {
    uint8_t  ch;
    uint8_t  flags;   /* reserved (bold/inverse later) */
    uint32_t fg;      /* renderer-specific - composed FB pixel for VESA */
    uint32_t bg;
} vt_cell_t;

typedef struct vt_buf {
    uint32_t   cols, rows;
    uint32_t   cur_col, cur_row;
    uint32_t   fg, bg;        /* current attribute for incoming chars */
    vt_cell_t *cells;         /* heap, row-major: cells[row*cols + col] */
} vt_buf_t;
```

## API

| Function | Purpose |
|---|---|
| `vt_init(vt, cols, rows, fg, bg)` | Allocate grid on heap; init all cells to space-on-bg. |
| `vt_putchar(vt, c)` | Write `c` at cursor, advance, scroll. Honours `\n`, `\r`, `\b`. Returns `vt_dirty_t` so the renderer knows whether to paint one cell or repaint the whole grid (scroll). |
| `vt_put_at(vt, c, col, row)` | Write `c` at fixed cell, no cursor move. |
| `vt_set_color(vt, fg, bg)` | Update current attribute. |
| `vt_set_cursor(vt, col, row)` | Move cursor; clamped to grid. |
| `vt_clear(vt)` | Fill grid with space-on-bg; reset cursor. |
| `vt_get_cell(vt, col, row)` | Read-only cell access (bounds-checked). |

## Renderer integration

vesa_tty.c routes the legacy global API (`vesa_tty_putchar` etc.) through the
calling task's `vt_buf_t`:

1. Write update to the backing grid.
2. If the grid matches `vtty_buf_focused()`, paint only the cell that changed
   (or the full grid on scroll) to the framebuffer.
3. If not focused, do nothing visible - the grid will surface on the next
   `vtty_switch`.

The VGA-text fallback path (tty.c) currently shares one buffer across all
TTYs; the per-TTY routing is a follow-up.

See also: [vtty](vtty.md), [vesa_tty](vesa_tty.md).
