---
title: debug
parent: Kernel subsystems
grand_parent: Reference
---

# debug - GDB-aware exception handlers + the fail-safe kernel panic

**Header:** `kernel/include/kernel/debug.h`  
**Source:** `kernel/arch/i386/debug/debug.c`

Two things live here: the GDB-aware handlers for the debug/breakpoint and fault
vectors (INT 1/3 and INT 8/13/14), and the **fail-safe kernel panic** — the
screen Makar drops to when something unrecoverable happens. When QEMU runs with a
GDB stub (`-s`), the debug exceptions are typically intercepted at the
hardware-emulation layer before they reach the kernel; these handlers are the
safety net for when no debugger is attached or the exception is raised by kernel
code directly.

---

## Functions

### `init_debug_handlers`

```c
void init_debug_handlers(void);
```

Register the GDB-aware debug/breakpoint handlers on vectors 1 (INT 1 - debug /
single-step) and 3 (INT 3 - software breakpoint), plus fault handlers on vectors
8 / 13 / 14 (double-fault, general-protection fault, page-fault) that route to the
kernel panic. Every IDT vector 0-31 is installed (here or by the ISR layer), so
no CPU exception goes unhandled.

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

## Kernel panic (fail-safe)

When something unrecoverable happens, Makar renders a panic screen and stops in a
way that never leaves a silent "CPU disabled" hang. Entry points (`debug.h`):

| API | Use |
|---|---|
| `kpanic(msg)` | Unconditional panic; renders the screen, then halts. Never returns. |
| `kpanic_at(msg, file, func, line)` | As above, recording the call site for the screen. |
| `KPANIC(msg)` | Macro — captures `__FILE__` / `__func__` / `__LINE__` automatically. |

A ring-3 fault becomes `SIGSEGV` (the process dies, not the kernel); a ring-0
fault, or an explicit `KPANIC`, lands here. The path is built to stay legible no
matter what failed:

- **VGA text, not the framebuffer.** `force_vga_text_mode()` turns the SVGA II
  engine off (`video_svga2_to_vga`), disables Bochs DISPI (`bochs_vbe_disable`),
  unblanks the attribute controller, and programs VGA mode 3 — then
  `render_panic_vga` writes straight to `0xB8000`. The framebuffer can be the very
  thing that failed, so the panic never depends on it. (`render_panic_vesa`, a
  framebuffer renderer, is kept `__attribute__((unused))` for a future UEFI/GOP
  target with no VGA text mode.)
- **Names the culprit.** A prominent **`Faulting app: <name> (pid N)`** line
  (highlighted) names the process the CPU was running — for a ring-0 syscall
  fault that's the userspace app the kernel was working on behalf of (e.g. a
  corrupt `SYS_*` path triggered by `mxweb`), with `- fault in ring 0/3` noting
  the privilege level. A **Resolve** line prints `addr2line -e makar.kernel
  <EIP>` so the faulting instruction maps to an exact `file.c:line` offline.
- **Spin, never `cli; hlt`.** `panic_halt()` drains the 8042, then **spins**
  polling the keyboard controller — a `cli; hlt` makes hypervisors report the CPU
  "disabled" and grab the host mouse. A fresh key *press* reboots via the 8042
  CPU-reset pulse (`outb(0x64, 0xFE)`), with a triple-fault fallback.
- **15-second auto-reboot watchdog.** So a panic never wedges a headless / CI
  box forever, `panic_halt()` also reboots itself after `PANIC_REBOOT_SECS`
  (15 s) — a key press still reboots immediately. The countdown uses the **CMOS
  RTC** (`rtc_unix_time`) as its clock, because it is pure port I/O and keeps
  advancing with interrupts masked (the PIT timer IRQ is dead during a panic, so
  `timer_get_ticks()` would never move). The remaining seconds repaint once per
  second on the bottom VGA-text line and on serial (`panic: rebooting in Ns`).
  If the RTC read fails it falls back to the classic wait-for-key behaviour.

A panic can also be triggered on purpose, for testing, with the
**`Ctrl+Alt+Shift+P`** keyboard chord (see [keyboard](keyboard.md)), or the
`panic [msg]` command in the in-kernel rescue shell.

---

## Future work

- Add a minimal kernel debugger (command loop over the serial port) that
  activates when INT 3 is hit without a GDB stub - letting a developer inspect
  state over a serial cable without a separate debugger binary.

(The earlier "map the faulting EIP to a function name" item shipped: the panic's
**Resolve** line emits an `addr2line` command for offline symbolisation.)
