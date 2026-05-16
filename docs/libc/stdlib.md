---
title: stdlib
parent: libc
---

# stdlib - Standard utilities

**Header:** `libc/include/stdlib.h`  
**Source:** `libc/stdlib/abort.c`

Provides the minimal subset of `<stdlib.h>` needed by the freestanding kernel
environment.

---

## Functions

### `abort`

```c
__attribute__((__noreturn__)) void abort(void);
```

Terminate the kernel abnormally.  In a hosted environment this would raise
`SIGABRT`; in Makar's freestanding environment it disables interrupts and
halts the CPU.

Marked `__noreturn__` so the compiler can omit unreachable code after a call
to `abort()`.

---

## Future work

- Add `atoi` / `atol` and `strtol` / `strtoul` for parsing numeric strings
  from shell commands and configuration files.
- Add `qsort` and `bsearch` for sorted data structures.
