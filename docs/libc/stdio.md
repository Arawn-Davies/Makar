---
title: stdio
parent: libc
---

# stdio - Standard I/O

**Header:** `libc/include/stdio.h`  
**Sources:** `libc/stdio/printf.c`, `libc/stdio/putchar.c`, `libc/stdio/puts.c`

Provides the minimal subset of `<stdio.h>` needed by the kernel.

---

## Constants

```c
#define EOF (-1)
```

Returned by character-output functions on failure.

---

## Functions

### `putchar`

```c
int putchar(int c);
```

Write the character `(char)c` to the terminal.  The concrete implementation
is provided by the kernel's `tty.c` via the linker (not in libc itself),
which calls `t_putchar`.  Returns `c` on success, `EOF` on failure.

This is the single output primitive on which `printf` and `puts` are built.

### `puts`

```c
int puts(const char *s);
```

Write the null-terminated string `s` to the terminal followed by a newline
(`'\n'`).  Returns a non-negative value on success, `EOF` on failure.

### `printf`

```c
int printf(const char *restrict format, ...);
```

Write a formatted string to the terminal.  Returns the number of characters
written, or `-1` on error.

**Supported format specifiers:**

| Specifier | Description |
|---|---|
| `%c` | Single character (`int` argument promoted from `char`). |
| `%s` | Null-terminated string. |
| `%%` | Literal `%` character. |

Unsupported specifiers (e.g. `%d`, `%x`, `%u`) are passed through verbatim
as a fallback - the format string from the first unrecognised `%` is printed
unchanged.

---

## Future work

- Add `%d` / `%i` (signed decimal), `%u` (unsigned decimal), `%x` / `%X`
  (hexadecimal), and `%p` (pointer) specifiers to `printf`.
- Add `snprintf` / `vsnprintf` for safe formatted output into buffers, needed
  by subsystems that build strings before writing them to the terminal or
  serial port.
