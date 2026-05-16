---
title: vesa_tty
parent: Kernel subsystems
---

# vesa_tty - VESA bitmap-font text renderer

**Header:** `kernel/include/kernel/vesa_tty.h`  
**Source:** `kernel/arch/i386/vesa_tty.c`  
**Font data:** `kernel/include/kernel/vesa_font.h`

Renders text onto the VESA linear framebuffer using the built-in 8×8 bitmap
font (`FONT8x8`).  Every glyph is scaled 2× (`FONT_SCALE = 2`), producing
16×16 cell characters for improved legibility at typical VESA resolutions.

This renderer runs in parallel with the VGA text-mode terminal: every
character written via the [tty](tty.md) driver is also forwarded here, so
both outputs show identical content.

---

## Geometry

| Symbol | Formula | Typical value (1024×768) |
|---|---|---|
| `FONT_CELL_W` | `8 × FONT_SCALE` = 16 px | 16 px |
| `FONT_CELL_H` | `8 × FONT_SCALE` = 16 px | 16 px |
| `tty_cols` | `fb.width / FONT_CELL_W` | 64 columns |
| `tty_rows` | `fb.height / FONT_CELL_H` | 48 rows |

---

## Functions

### `vesa_tty_init`

```c
bool vesa_tty_init(void);
```

1. Call `vesa_get_fb()` - returns `false` immediately if no framebuffer is
   available.
2. Call `paging_map_region()` to map the framebuffer physical address range
   into the virtual address space before any pixel write.
3. Compute `tty_cols` and `tty_rows` from the framebuffer dimensions.
4. Set foreground to white (`0xFFFFFF`) and background to black (`0x000000`).
5. Call `vesa_clear(tty_bg)` to erase any BIOS/bootloader splash.
6. Mark the renderer as ready.

Must be called after `vesa_init()` and `paging_init()`.

### `vesa_tty_is_ready`

```c
bool vesa_tty_is_ready(void);
```

Return `true` once `vesa_tty_init()` has completed successfully.  All other
`vesa_tty_*` functions check this flag and return immediately if not ready,
making them safe to call at any point during boot.

### `vesa_tty_putchar`

```c
void vesa_tty_putchar(char c);
```

Render character `c` at the current cursor position and advance the cursor.

- `'\n'` - move to column 0 of the next row; scroll up one row if the
  cursor reaches the bottom.
- `'\r'` - move to column 0 of the current row.
- `'\b'` - erase the character to the left of the cursor (draw a space).
- Any other character - draw the glyph and advance the column; wrap to the
  next row at the right edge, scrolling if necessary.

### `vesa_tty_setcolor`

```c
void vesa_tty_setcolor(uint32_t fg_rgb, uint32_t bg_rgb);
```

Set the foreground and background colours as 24-bit `0x00RRGGBB` values.
The colours are converted to the framebuffer's native packed format via the
RGB shift fields in `vesa_fb_t`.

### `vesa_tty_put_at`

```c
void vesa_tty_put_at(char c, uint32_t col, uint32_t row);
```

Draw character `c` at cell (`col`, `row`) without moving the cursor.  Used
by the spinner animation.  Out-of-bounds cell coordinates are silently
ignored.

### `vesa_tty_spinner_tick`

```c
void vesa_tty_spinner_tick(uint32_t tick);
```

Draw one frame of a four-character spinner (`|`, `/`, `-`, `\`) at the
top-right corner of the display.  The frame advances every 12 ticks.
Mirrors `t_spinner_tick()` on the VESA display.

---

## Scrolling

When the cursor moves past the last row, `scroll_up()` shifts all pixel rows
up by `FONT_CELL_H` scanlines using `memcpy`, then clears the bottom character
row.  This is a simple pixel-copy scroll with no hardware acceleration.

---

## Future work

- Hardware cursor overlay or a software block cursor to show the text
  insertion point.
- Colour attributes per character cell (already supported by `vesa_tty_setcolor`
  globally; needs per-cell storage).
- Scalable or anti-aliased fonts (TrueType / SFN) for higher-resolution
  displays.
- Optimised scroll using `REP MOVSD` or the framebuffer's hardware scroll
  register where available.
