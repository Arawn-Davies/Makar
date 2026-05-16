---
title: isr
parent: Kernel subsystems
---

# isr - Interrupt and IRQ dispatch

**Header:** `kernel/include/kernel/isr.h`  
**Source:** `kernel/arch/i386/isr.c`  
**ASM stubs:** `kernel/arch/i386/isr_asm.S`

Provides a thin, handler-table-based dispatch layer on top of the IDT.
C code registers and unregisters callbacks for any interrupt vector; the ASM
stubs push a unified register frame onto the stack and call the C dispatcher.

---

## IRQ vector constants

Hardware IRQs are remapped by `init_descriptor_tables()` so that IRQ 0–15
occupy IDT vectors 32–47, well above the CPU exception range (0–31).

| Constant | Vector | Hardware source |
|---|---|---|
| `IRQ0` | 32 | PIT timer channel 0 |
| `IRQ1` | 33 | PS/2 keyboard |
| `IRQ2` | 34 | Cascade (slave PIC) |
| `IRQ3` | 35 | COM2 / COM4 serial |
| `IRQ4` | 36 | COM1 / COM3 serial |
| `IRQ5` | 37 | LPT2 / sound card |
| `IRQ6` | 38 | Floppy disk |
| `IRQ7` | 39 | LPT1 / spurious |
| `IRQ8` | 40 | RTC |
| `IRQ9` | 41 | ACPI / free |
| `IRQ10` | 42 | Free |
| `IRQ11` | 43 | Free |
| `IRQ12` | 44 | PS/2 mouse |
| `IRQ13` | 45 | FPU / coprocessor |
| `IRQ14` | 46 | Primary ATA |
| `IRQ15` | 47 | Secondary ATA |

---

## Data structures

### `registers_t`

```c
typedef struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;
```

Snapshot of CPU state at the moment an interrupt or exception fires.  The
ASM stubs push the processor-saved fields (`eip`, `cs`, `eflags`, `useresp`,
`ss`) plus `pusha` output, `ds`, and the vector number / error code, then
pass a pointer to this structure to the C handler.

---

## Type aliases

```c
typedef void (*isr_t)(registers_t);
```

Function-pointer type for interrupt callbacks.  The handler receives a copy
(not a pointer) of the register state at interrupt time.

---

## Functions

### `init_isr_handlers`

```c
void init_isr_handlers(void);
```

Zero-initialise the 256-entry handler table.  Called from
`init_descriptor_tables()`.

### `register_interrupt_handler`

```c
void register_interrupt_handler(uint8_t n, isr_t handler);
```

Install `handler` as the callback for vector `n`.  Prints a confirmation
message to the terminal and serial port.  For IRQ vectors (`n >= IRQ0`) the
message identifies the IRQ number; for exception vectors it identifies the
interrupt number.

### `unregister_interrupt_handler`

```c
void unregister_interrupt_handler(uint8_t n);
```

Remove the callback for vector `n` by setting its slot to `NULL`.

### `is_registered`

```c
int is_registered(uint8_t n);
```

Return non-zero if a callback is currently registered for vector `n`.

---

## Dispatch flow

**Exceptions (vectors 0–31):** If a handler is registered, it is called.
Otherwise `isr_handler` calls `PANIC("Unhandled Interrupt")`.

**IRQs (vectors 32–47):** If a handler is registered, it is called.
Regardless of whether a handler ran, `irq_handler` sends an End-Of-Interrupt
(`EOI`) signal to the master 8259 PIC (and also to the slave if the IRQ
originated from the slave, i.e. vector ≥ 40).
