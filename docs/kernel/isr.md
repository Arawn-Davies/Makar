---
title: isr
parent: Kernel subsystems
grand_parent: Reference
---

# isr - Interrupt, exception, IRQ, and syscall dispatch

**Header:** `kernel/include/kernel/isr.h`  
**Source:** `kernel/arch/i386/core/isr.c`
**ASM stubs:** `kernel/arch/i386/core/isr_asm.S`

Provides a thin, handler-table-based dispatch layer on top of the IDT.
C code registers and unregisters callbacks for any interrupt vector; the ASM
stubs push a unified register frame onto the stack and call the C dispatcher.

The same mechanics cover CPU exceptions, remapped 8259 IRQs, and the `int
0x80` syscall gate. The descriptor-table layer decides which vectors user mode
may enter; this layer decides which C handler receives the saved register
frame.

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

For interrupts that arrive from ring 0, `useresp` and `ss` are not meaningful
processor-saved user-stack fields; handlers should check the saved `cs` ring
when they need to distinguish kernel and user faults.

The stubs reload `ds`, `es`, and `fs` to the kernel data selector while running
C code. They deliberately preserve `%gs`, because user tasks may use `%gs` for
Linux i386 TLS after `set_thread_area`. Kernel C must not rely on `%gs` for
ordinary data access.

---

## Type aliases

```c
typedef void (*isr_t)(registers_t *);
```

Function-pointer type for interrupt callbacks. The handler receives a pointer
to the register frame assembled by the stub. Syscall and signal code may update
fields in that frame before returning to user mode.

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
Otherwise `isr_handler` calls `PANIC("Unhandled Interrupt")`. Page fault
handling is registered on vector 14 and is allowed to resolve user COW faults;
unhandled faults still panic with CR2 and error-code diagnostics.

**IRQs (vectors 32–47):** If a handler is registered, it is called.
Regardless of whether a handler ran, `irq_handler` sends an End-Of-Interrupt
(`EOI`) signal to the master 8259 PIC (and also to the slave if the IRQ
originated from the slave, i.e. vector ≥ 40).

**Syscalls (vector 128):** The IDT installs vector `0x80` as a DPL=3 interrupt
gate, so ring-3 code may execute `int $0x80`. The syscall stub builds the same
kind of `registers_t` frame and dispatches into the syscall layer, which treats
the general-purpose registers as Linux i386 syscall arguments.

## Register-frame conventions

The interrupt stubs make three conventions important to later subsystems:

| Convention | Reason |
|---|---|
| All C handlers receive `registers_t *`. | Handlers can inspect and, where required, modify the return frame. |
| `%gs` is preserved. | Hosted libc and TLS-aware programs keep the thread pointer in `%gs`. |
| IRQ handlers always send EOI after callback dispatch. | Device drivers do not need to duplicate PIC acknowledgement logic. |

Handlers should keep IRQ work short. Keyboard VT switching, for example,
records a pending repaint and lets task context perform the framebuffer work
later, because painting thousands of pixels inside IRQ1 can cause lost input.
