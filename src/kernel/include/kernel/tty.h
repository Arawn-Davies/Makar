#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Current number of VGA text rows (25 or 50). */
extern size_t t_height;

/* Current cursor position (0-based). */
extern size_t t_row;
extern size_t t_column;

/*
 * Write character c with the given colour attribute directly into the VGA
 * buffer at cell (x, y) without moving the cursor.
 */
void t_putentryat(char c, uint8_t color, size_t x, size_t y);

void terminal_initialize(void);
void terminal_set_rows(size_t rows);
void t_setcolor(uint8_t color);
void t_backspace();
void t_putchar(char c);
void t_write(const char *data, size_t size);
void t_writestring(const char *data);

/* Set while the (non-verbose) boot loading screen owns the framebuffer: t_putchar
 * then keeps text off the framebuffer (VGA buffer + serial still record it) so
 * concurrent background ktest output doesn't bleed over the logo/progress bar.
 * The loading screen draws the logo/bar via vesa_tty_put_at, which is unaffected. */
extern volatile int g_boot_loading;

void t_hex(uint32_t num);
void t_dec(uint32_t num);

void t_spinner_tick(uint32_t tick);

/*
 * Set the VGA terminal colour attribute to `color`, clear the screen to that
 * background, and reset the cursor to (0, 0).  Use this to apply a whole-
 * terminal colour scheme change (e.g. switching from the boot white-on-black
 * to the Medli-style white-on-blue shell theme).
 */
void terminal_set_colorscheme(uint8_t color);

/*
 * Return the usable terminal height in rows.  When the VESA framebuffer TTY
 * is active this is the VESA row count; otherwise it is t_height (VGA text).
 */
int t_get_rows(void);

/* Move the VGA hardware cursor and VESA software cursor to (col, row). */
void t_set_cursor(size_t col, size_t row);

/* Fill the screen with spaces using colour attribute clr, reset cursor. */
void t_fill(uint8_t clr);

#endif
