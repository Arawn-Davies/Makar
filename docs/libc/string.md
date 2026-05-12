# string - Memory and string utilities

**Header:** `libc/include/string.h`  
**Sources:** `libc/string/memcmp.c`, `memcpy.c`, `memmove.c`, `memset.c`, `strlen.c`

Provides the core `<string.h>` memory and string functions required by the
kernel and its libc.  All are standard C implementations with no OS-specific
dependencies.

---

## Functions

### `memset`

```c
void *memset(void *s, int c, size_t n);
```

Fill the first `n` bytes of memory at `s` with the byte value `(unsigned char)c`.
Returns `s`.  Used throughout the kernel to zero buffers, the IDT, ISR handler
table, and PMM bitmap.

### `memcpy`

```c
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
```

Copy `n` bytes from `src` to `dst`.  The source and destination regions must
not overlap; use `memmove` if they may.  Returns `dst`.

### `memmove`

```c
void *memmove(void *dst, const void *src, size_t n);
```

Copy `n` bytes from `src` to `dst`, handling overlapping regions correctly by
choosing the copy direction based on the relative positions of `dst` and `src`.
Returns `dst`.  Used by the VESA TTY scroll routine.

### `memcmp`

```c
int memcmp(const void *s1, const void *s2, size_t n);
```

Compare the first `n` bytes of `s1` and `s2`.  Returns 0 if equal, a
negative value if `s1 < s2`, or a positive value if `s1 > s2`.

### `strlen`

```c
size_t strlen(const char *s);
```

Return the number of characters in the null-terminated string `s`, not
including the terminating `'\0'`.

---

## Future work

- Add `strcpy`, `strncpy`, `strcmp`, `strncmp`, `strchr`, `strrchr`,
  `strstr` for the shell and configuration parser.
- Add `strtok` / `strtok_r` for tokenising shell command lines.
