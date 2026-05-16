---
title: debug
parent: Kernel subsystems
---

# debug - INT 1 / INT 3 debug-exception handlers

**Header:** `kernel/include/kernel/debug.h`  
**Source:** `kernel/arch/i386/debug.c`

Installs graceful fallback handlers for the two CPU debug exceptions.  When
QEMU is running with a GDB stub (`-s`), these exceptions are typically
intercepted at the hardware-emulation layer before they reach the kernel.
These handlers act as a safety net for the case where no debugger is attached
or the exceptions are triggered by kernel code directly.

---

## Functions

### `init_debug_handlers`

```c
void init_debug_handlers(void);
```

Register `debug_exception_handler` on vector 1 (INT 1 - debug / single-step)
and `breakpoint_handler` on vector 3 (INT 3 - software breakpoint).

Called from `kernel_main` immediately after `init_descriptor_tables()`.

---

## Exception behaviour

Both handlers follow the same pattern:

1. Print an identification line to the VGA terminal and the serial port.
2. Dump all general-purpose registers, `EIP`, `EFLAGS`, `CS`, `SS`, `ESP`,
   and `EBP` to both outputs.
3. **Return normally** - execution resumes at the instruction after the trap.

This means the kernel does not panic on a debug exception.  If QEMU's GDB
stub is active, control is passed to the debugger before the kernel handler
runs, so the handler is usually invisible during a debugging session.

---

## Register dump format

Each register is printed on its own line in the format `REG   =0xXXXXXXXX`
to the VGA terminal.  The serial port receives a plain `0xXXXXXXXX\n` line
per register, formatted for easy grepping in the serial log.

---

## INT 1 - Debug exception (vector 1)

Fired by:
- Hardware single-step mode (EFLAGS.TF set).
- Hardware data/instruction breakpoints (DR0–DR3).

In a GDB session, QEMU intercepts this and notifies the debugger.  The kernel
handler prints `[DEBUG] INT1 debug exception` and the register dump if it
is ever reached without a debugger.

## INT 3 - Breakpoint (vector 3)

Fired by:
- The `INT3` instruction (opcode `0xCC`), which GDB inserts as a software
  breakpoint.
- An explicit `__asm__ volatile ("int $3")` in kernel code.

The kernel handler prints `[DEBUG] INT3 breakpoint hit` and the register
dump.

---

## Future work

- Add a minimal kernel debugger (command loop over the serial port) that
  activates when INT 3 is hit without a GDB stub - letting a developer
  inspect state over a serial cable without a separate debugger binary.
- Log the faulting `EIP` against the kernel symbol table to print a
  function name in the register dump.
