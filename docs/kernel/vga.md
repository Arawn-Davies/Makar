---
title: vga
parent: Kernel subsystems
---

# vga - VGA text-mode constants and low-level helpers

**Header:** `kernel/include/kernel/vga.h`

Definitions and `static inline` helpers for the 80×25 VGA text-mode buffer
at physical address `0xB8000`.  These are used by the [tty](tty.md) driver.

---

## Constants

| Constant | Value | Description |
|---|---|---|
| `VGA_WIDTH` | `80` | Text columns per row. |
| `VGA_HEIGHT` | `25` | Text rows on screen. |
| `VGA_MEMORY` | `0xB8000` | Base address of the VGA text buffer. |

---

## Enum - `vga_color`

The 16 standard CGA/VGA palette indices:

| Name | Value |
|---|---|
| `COLOR_BLACK` | 0 |
| `COLOR_BLUE` | 1 |
| `COLOR_GREEN` | 2 |
| `COLOR_CYAN` | 3 |
| `COLOR_RED` | 4 |
| `COLOR_MAGENTA` | 5 |
| `COLOR_BROWN` | 6 |
| `COLOR_LIGHT_GREY` | 7 |
| `COLOR_DARK_GREY` | 8 |
| `COLOR_LIGHT_BLUE` | 9 |
| `COLOR_LIGHT_GREEN` | 10 |
| `COLOR_LIGHT_CYAN` | 11 |
| `COLOR_LIGHT_RED` | 12 |
| `COLOR_LIGHT_MAGENTA` | 13 |
| `COLOR_LIGHT_BROWN` | 14 |
| `COLOR_WHITE` | 15 |

---

## Functions

### `make_color`

```c
static inline uint8_t make_color(enum vga_color fg, enum vga_color bg);
```

Pack a foreground and background colour into a single VGA attribute byte
(`bg << 4 | fg`).

### `make_vgaentry`

```c
static inline uint16_t make_vgaentry(char c, uint8_t color);
```

Combine an ASCII character and a VGA attribute byte into the 16-bit value
written to the VGA text buffer (`color << 8 | c`).

### `update_cursor`

```c
static inline void update_cursor(size_t row, size_t col);
```

Move the hardware text cursor to the cell at (`col`, `row`) by writing the
linear position to VGA I/O ports `0x3D4`/`0x3D5`.
