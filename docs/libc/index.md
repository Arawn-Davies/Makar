# libc - Freestanding standard library

Makar ships a minimal freestanding C library (`libc/`) that provides just
enough of the standard C interface for the kernel to compile against.  It is
**not** a full POSIX libc - it exists solely to satisfy the `#include`
dependencies of the kernel and its subdirectories.

---

## Modules

| Header | Source(s) | Description |
|---|---|---|
| [`stdio.h`](stdio.md) | `stdio/printf.c`, `putchar.c`, `puts.c` | Formatted and character output. |
| [`stdlib.h`](stdlib.md) | `stdlib/abort.c` | Abnormal termination. |
| [`string.h`](string.md) | `string/mem*.c`, `strlen.c` | Memory and string utilities. |
| `sys/cdefs.h` | - | Compiler/libc identification macros. |

---

## `sys/cdefs.h`

```c
#define __makar_libc 1
```

Identifies this as the Makar freestanding libc.  Code that needs to detect
the Makar environment at compile time can test `#ifdef __makar_libc`.

---

## Design notes

- All sources are compiled with `-ffreestanding` - no hosted runtime is
  available.
- `putchar` is the single output primitive; `printf` and `puts` are built on
  top of it.  The kernel's `tty.c` supplies the concrete `putchar`
  implementation via the linker.
- No dynamic memory allocation (`malloc`/`free`) is provided here; the
  kernel's own `kmalloc`/`kfree` heap is used instead.
- No floating-point, no locale, no signal handling, no file I/O beyond the
  terminal.

---

## Future work

- Add `<stdarg.h>`-compatible `vprintf` / `snprintf` for safer formatted
  output in kernel subsystems.
- Extend `printf` to support `%d`, `%u`, `%x`, `%p` format specifiers fully.
- Consider a kernel-space `<errno.h>` for richer error reporting from syscall
  stubs.
