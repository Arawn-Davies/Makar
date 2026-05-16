---
title: system
parent: Kernel subsystems
---

# system - Panic, halt, and assertion helpers

**Headers:** `kernel/include/kernel/system.h`  
**Source:** `kernel/arch/i386/system.c`

Low-level CPU control and unrecoverable-error reporting.

---

## Macros

### `PANIC(msg)`

```c
#define PANIC(msg) panic(msg, __FILE__, __LINE__);
```

Convenience wrapper around `panic()` that automatically supplies the current
source file and line number.

### `ASSERT(b)`

```c
#define ASSERT(b) ((b) ? (void)0 : panic_assert(__FILE__, __LINE__, #b));
```

Evaluates expression `b`.  If `b` is false, calls `panic_assert()` with the
file, line, and stringified expression.  In non-debug builds the expression
is still evaluated (no `NDEBUG` guard), so do not use it with side effects.

---

## Functions

### `io_wait`

```c
void io_wait(void);
```

Inserts a tiny delay (approximately 400 ns) by executing two dummy jumps.
Used when communicating with slow legacy hardware (e.g. ATA/ATAPI, 8259 PIC)
where a brief pause is required between consecutive port writes.

### `halt`

```c
void halt(void);
```

Executes the `HLT` instruction, stopping the CPU until the next interrupt.
Called after an unrecoverable exception or assertion failure to freeze
execution.  Typically invoked inside an infinite loop so the CPU never
returns to the faulting code.

### `panic`

```c
void panic(char *msg, char *file, uint32_t line);
```

Handles an unrecoverable kernel error.  Disables interrupts (`CLI`), prints a
formatted message to the VGA terminal, then spins forever.

| Parameter | Description |
|---|---|
| `msg` | Human-readable error description. |
| `file` | Source file name (typically `__FILE__`). |
| `line` | Source line number (typically `__LINE__`). |

### `panic_assert`

```c
void panic_assert(char *file, uint32_t line, char *desc);
```

Like `panic()`, but formats the output as a failed assertion.  Called
automatically by the `ASSERT()` macro.

| Parameter | Description |
|---|---|
| `file` | Source file name. |
| `line` | Source line number. |
| `desc` | Stringified assertion expression. |
