# asm - Inline x86 port I/O and CPU-control helpers

**Header:** `kernel/include/kernel/asm.h`

All functions in this header are declared `inline` and consist of a single
`asm volatile` statement.  They compile to one or two instructions with no
function-call overhead.

---

## Functions

### `outb`

```c
void outb(uint16_t port, uint8_t val);
```

Write byte `val` to I/O port `port` using the x86 `OUT` instruction.
Marked `volatile` to prevent the compiler from reordering or eliminating the
write.

### `inb`

```c
uint8_t inb(uint16_t port);
```

Read and return one byte from I/O port `port` using the x86 `IN` instruction.

### `enable_interrupts`

```c
void enable_interrupts(void);
```

Execute `STI` - sets the CPU interrupt-enable flag, allowing hardware
interrupts to be delivered.  Call only after the IDT is fully configured.

### `disable_interrupts`

```c
void disable_interrupts(void);
```

Execute `CLI` - clears the CPU interrupt-enable flag, masking all maskable
hardware interrupts.  Used in critical sections and panic handlers.

### `invlpg`

```c
void invlpg(void *m);
```

Invalidate the TLB entry for the virtual address `m` using the `INVLPG`
instruction.  Must be called whenever a page-directory or page-table entry is
modified while paging is active, to ensure the CPU does not use a stale
cached mapping.
