# tty - VGA text terminal driver

**Header:** `kernel/include/kernel/tty.h`  
**Source:** `kernel/arch/i386/tty.c`

Provides a simple 80×25 VGA text-mode terminal.  Each write is mirrored to
the [VESA TTY](vesa_tty.md) renderer when it is available, so the same text
appears on both outputs.

The driver maintains a per-row fill counter (`t_line_fill`) so that
backspace can retreat to the correct column even after a line wrap.

---

## Functions

### `terminal_initialize`

```c
void terminal_initialize(void);
```

Clear the VGA text buffer, reset the cursor to position (0, 0), and set the
default colour to white-on-black.  Must be called before any other `t_`
function.

### `t_putchar`

```c
void t_putchar(char c);
```

Write a single character to the terminal at the current cursor position and
advance the cursor.  A newline (`'\n'`) moves to the start of the next row.
If the cursor reaches the bottom of the screen, `t_scroll()` is called to
shift all rows up by one.  Also forwards `c` to `vesa_tty_putchar()`.

### `t_backspace`

```c
void t_backspace(void);
```

Erase the character to the left of the cursor and move the cursor back one
position.  Handles the case where the cursor is at the start of a row by
retreating to the end of the previous row using the saved `t_line_fill` value.

### `t_write`

```c
void t_write(const char *data, size_t size);
```

Write `size` bytes from `data` to the terminal by calling `t_putchar()` for
each byte.

### `t_writestring`

```c
void t_writestring(const char *data);
```

Write the null-terminated string `data` to the terminal.  Equivalent to
`t_write(data, strlen(data))`.

### `t_hex`

```c
void t_hex(uint32_t num);
```

Print `num` as an unsigned hexadecimal integer (no `0x` prefix).

### `t_dec`

```c
void t_dec(uint32_t num);
```

Print `num` as an unsigned decimal integer.

### `t_spinner_tick`

```c
void t_spinner_tick(uint32_t tick);
```

Render one frame of a four-character spinner (`|`, `/`, `-`, `\`) in the
top-right corner of the VGA buffer.  The frame is chosen from `tick` so the
animation advances roughly every 12 timer ticks.  Also calls
`vesa_tty_spinner_tick()` to update the VESA display.

Called from the PIT timer callback on every tick to give a live visual
indication that the kernel has not stalled.
