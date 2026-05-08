# timer — PIT timer driver and `ksleep`

**Header:** `kernel/include/kernel/timer.h`  
**Source:** `kernel/arch/i386/hardware/timer.c`

Programs the Intel 8253/8254 Programmable Interval Timer (PIT) channel 0 to
fire IRQ 0 at a configurable frequency, and provides a busy-wait sleep
function built on the resulting tick counter.

**OSDev reference:** [Programmable Interval Timer (PIT)](https://wiki.osdev.org/PIT)

---

## PIT I/O ports

| Port | Direction | Description |
|---|---|---|
| `0x40` | R/W | Channel 0 data — divisor (lo byte then hi byte). |
| `0x41` | R/W | Channel 1 data (unused; historically DRAM refresh). |
| `0x42` | R/W | Channel 2 data (unused; drives PC speaker). |
| `0x43` | Write | Mode/Command register. |

---

## How it works

The PIT's input clock runs at **1 193 180 Hz** (derived from the original
IBM PC 14.318 MHz crystal divided by 12).  To achieve a desired frequency `f`,
a 16-bit divisor `1193180 / f` is written to PIT channel 0 (port `0x40`).
The PIT then fires IRQ 0 every `divisor` clock cycles.

The command byte `0x36` written to port `0x43` selects:
- Channel 0 (bits 7–6 = `00`)
- Lo-byte then hi-byte access (bits 5–4 = `11`)
- Mode 3: square-wave generator (bits 3–1 = `011`)
- Binary counting (bit 0 = `0`)

At boot Makar calls `init_timer(100)`, giving a tick rate of **100 Hz** (one
tick every 10 ms).

Each IRQ 0 fires `timer_callback`, which:

1. Increments the global `tick` counter.
2. Calls `t_spinner_tick(tick)` to animate the terminal spinner.
3. Every `SCHED_QUANTUM = 4` ticks (≈ 80 ms at 50 Hz), sends End-Of-Interrupt
   to the master PIC and calls `task_yield()`. This drives **preemptive task
   switching** — a busy-loop ring-0 task that never voluntarily yields will
   still surrender the CPU at the next quantum boundary. EOI is sent before
   the yield so further timer IRQs can fire while the new task runs.

---

## Functions

### `init_timer`

```c
void init_timer(uint32_t frequency);
```

Configure PIT channel 0 to interrupt at `frequency` Hz and enable CPU
interrupts.

1. Registers `timer_callback` on `IRQ0`.
2. Computes the 16-bit divisor (`1193180 / frequency`).
3. Writes command byte `0x36` to port `0x43`.
4. Writes the divisor to port `0x40` (lo byte first, then hi byte).
5. Calls `enable_interrupts()` — this is the point at which hardware interrupts
   first become active in the boot sequence.

| Parameter | Description |
|---|---|
| `frequency` | Desired interrupt rate in Hz. Must divide evenly into 1 193 180 or the rate will be approximate. Maximum is 1 193 180 Hz (divisor = 1); minimum is ~18 Hz (divisor = 65535). |

### `timer_get_ticks`

```c
uint32_t timer_get_ticks(void);
```

Return the current tick count.  The counter starts at 0 and increments by 1
on every timer interrupt.  At 100 Hz it wraps after approximately 497 days
of continuous uptime.

### `ksleep`

```c
void ksleep(uint32_t ticks);
```

Busy-wait until at least `ticks` timer ticks have elapsed since the call.
At 100 Hz, `ksleep(100)` sleeps for approximately one second.

This is a spin-wait — the CPU executes a tight loop and does not yield.  It
is suitable for short post-boot delays but should be replaced with an
interrupt-driven sleep once a scheduler exists.

---

## Future work

- Replace the spin-wait in `ksleep` with an interrupt-driven sleep queue once
  a process scheduler is in place.
- Expose a monotonic clock interface (nanoseconds / seconds since boot) for
  use by higher-level subsystems and a future system-call layer.
- Add a one-shot alarm / callback registration API for kernel timers.
