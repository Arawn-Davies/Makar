#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/vga.h>
#include <kernel/vesa_tty.h>
#include <kernel/serial.h>

size_t t_line_fill[VGA_WIDTH];
size_t t_row;
size_t t_column;
uint8_t t_color;
uint16_t *t_buffer;
size_t t_height = VGA_HEIGHT;

void t_putentryat(char c, uint8_t color, size_t x, size_t y);
void t_scroll();

void terminal_initialize(void)
{
	t_row = 0;
	t_column = 0;
	t_color = make_color(COLOR_WHITE, COLOR_BLUE);
	t_buffer = VGA_MEMORY;
	for (size_t y = 0; y < t_height; y++)
	{
		for (size_t x = 0; x < VGA_WIDTH; x++)
		{
			const size_t index = y * VGA_WIDTH + x;
			t_buffer[index] = make_vgaentry(' ', t_color);
		}
	}
}

void t_backspace()
{
	vesa_tty_putchar('\b');

	if (t_column == 0)
	{
		if (t_row > 0)
		{
			t_row--;
		}
		t_column = t_line_fill[t_row];
	}
	else
	{
		t_column--;
	}

	t_putentryat(32, t_color, t_column, t_row);
	update_cursor(t_row, t_column);
}

void t_setcolor(uint8_t color)
{
	t_color = color;
}

void t_putentryat(char c, uint8_t color, size_t x, size_t y)
{
	const size_t index = y * VGA_WIDTH + x;
	t_buffer[index] = make_vgaentry(c, color);
}

void t_putchar(char c)
{
	if (g_serial_verbose)
		Serial_WriteChar(c);
	vesa_tty_putchar(c);

	if (c != '\n')
	{
		t_putentryat(c, t_color, t_column, t_row);
	}

	if (++t_column == VGA_WIDTH || c == '\n')
	{
		t_line_fill[t_row] = t_column - 1;
		t_column = 0;
		if (++t_row == t_height)
		{
			if (!vesa_tty_is_ready())
				t_scroll();
			else
				t_row = 0;
		}
	}

	if (!vesa_tty_is_ready())
		update_cursor(t_row, t_column);
}

void t_scroll()
{
	t_row--;
	for (size_t y = 0; y < t_height - 1; y++)
	{
		for (size_t x = 0; x < VGA_WIDTH; x++)
		{
			const size_t src_index = y * VGA_WIDTH + x;
			const size_t dst_index = (y + 1) * VGA_WIDTH + x;
			t_buffer[src_index] = t_buffer[dst_index];
		}
		t_line_fill[y] = t_line_fill[y + 1];
	}

	for (size_t x = 0; x < VGA_WIDTH; x++)
	{
		const size_t index = (t_height - 1) * VGA_WIDTH + x;
		t_buffer[index] = make_vgaentry(' ', t_color);
	}
}

void t_write(const char *data, size_t size)
{
	for (size_t i = 0; i < size; i++)
		t_putchar(data[i]);
}

void t_writestring(const char *data)
{
	t_write(data, strlen(data));
}

// Credit: lja83/OSDEV
void monitor_write_base(uint32_t num, uint32_t base)
{
	uint32_t baseNum;

	if(num > (base - 1))
	{
		monitor_write_base(num / base, base);
		baseNum = num % base;
	}
	else
	{
		baseNum = num;
	}

	if(baseNum > 9)
	{
		// Capital letters start at 65 but we have to
		// subtract 10 to get the offset
		t_putchar(baseNum + 65 - 10);
	}
	else
	{
		// Numbers start at 0x30 in ASCII
		t_putchar(baseNum + 0x30);
	}
}

void t_hex(uint32_t num)
{
	monitor_write_base(num, 16);
}

void t_dec(uint32_t num)
{
	monitor_write_base(num, 10);
}

// End credit

int t_get_rows(void)
{
	if (vesa_tty_is_ready())
		return (int)vesa_tty_get_rows();
	return (int)t_height;
}

void t_spinner_tick(uint32_t tick)
{
	static const char frames[] = {'|', '/', '-', '\\'};
	char c = frames[(tick / 12) % 4];
	t_putentryat(c, make_color(COLOR_WHITE, COLOR_BLACK), VGA_WIDTH - 1, 0);
	vesa_tty_spinner_tick(tick);
}

void terminal_set_colorscheme(uint8_t color)
{
	t_row    = 0;
	t_column = 0;
	t_color  = color;
	for (size_t y = 0; y < t_height; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			t_buffer[index] = make_vgaentry(' ', t_color);
		}
	}
	update_cursor(0, 0);
}

void t_set_cursor(size_t col, size_t row)
{
	update_cursor(row, col);
	if (vesa_tty_is_ready())
		vesa_tty_set_cursor((uint32_t)col, (uint32_t)row);
}

void t_fill(uint8_t clr)
{
	for (size_t y = 0; y < t_height; y++)
		for (size_t x = 0; x < VGA_WIDTH; x++)
			t_buffer[y * VGA_WIDTH + x] = make_vgaentry(' ', clr);
	t_row    = 0;
	t_column = 0;
	update_cursor(0, 0);
	if (vesa_tty_is_ready())
		vesa_tty_clear();
}

void terminal_set_rows(size_t rows)
{
	/* Only 25 and 50 are supported. */
	if (rows != 25 && rows != 50)
		return;

	/*
	 * Program CRTC register 9 (Max Scan Line).
	 * 80x25: 16 scanlines per char → bits[4:0] = 15
	 * 80x50:  8 scanlines per char → bits[4:0] =  7
	 *
	 * The CRTC address register for colour modes is at 0x3D4 and the
	 * data register is at 0x3D5.
	 */
	uint8_t scanlines = (rows == 50) ? 7 : 15;

	outb(0x3D4, 0x09);
	outb(0x3D5, (uint8_t)((inb(0x3D5) & 0xE0) | scanlines));

	/* Update cursor shape to span the bottom two lines of the new cell. */
	uint8_t cursor_start = scanlines - 1;
	uint8_t cursor_end   = scanlines;
	outb(0x3D4, 0x0A);
	outb(0x3D5, (uint8_t)((inb(0x3D5) & 0xC0) | cursor_start));
	outb(0x3D4, 0x0B);
	outb(0x3D5, (uint8_t)((inb(0x3D5) & 0xE0) | cursor_end));

	t_height = rows;
	terminal_initialize();
}
