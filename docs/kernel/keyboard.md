# keyboard - PS/2 keyboard driver

**Header:** `src/kernel/include/kernel/keyboard.h`
**Source:** `src/kernel/arch/i386/drivers/keyboard.c`

A layered, SMP-ready PS/2 keyboard driver. Handles IRQ 1, decodes scan-code
set 1 (including `e0`/`e1` prefixes and `make`/`break` separation), tracks
modifier state, and routes cooked bytes to either a per-task SPSC ring or
a global fallback ring.

This document describes the post-rewrite driver landed for slice #5 of the
`feat/tty-multitasking` follow-up roadmap. It replaces the older single-
state-flag implementation that suffered from "sticky e0", an unsynchronised
SPSC ring race, and a tearing slot table - see "Why the rewrite" below.

---

## Pipeline

```
   PS/2 controller (port 0x60/0x64)
              │
              ▼
       keyboard_irq_handler
              │   (drains controller, AUX/error filtering)
              ▼
         decoder_feed
              │   (state machine: NORMAL / AFTER_E0 / AFTER_E1A / AFTER_E1B)
              ▼
         apply_modifier  ──── tracks L/R shift, ctrl, alt; caps-lock toggle
              │
              ▼ (only on make events; break events stop here)
            on_make
              │   (Alt+Fn → vtty_switch; Ctrl-A prefix; Ctrl-C SIGINT)
              ▼
        translate_make
              │   (US QWERTY tables; Ctrl+letter → control code;
              │    arrows → KEY_ARROW_* sentinels)
              ▼
            kb_route
              │
              ▼
   ┌──────────┴──────────┐
   ▼                     ▼
per-task SPSC ring   global fallback ring
   (kb_slots[i])         (kb_buf[256])
```

Each stage is a pure function of its input plus the small amount of decoder
state (state machine + modifier flags). Make and break events are strictly
separated: a break (top bit set on the post-prefix byte) only updates
modifier state and never reaches the translator.

---

## Public API

| Function | Purpose |
|---|---|
| `keyboard_init()` | Register IRQ1 handler; drain any pre-existing controller queue. |
| `keyboard_getchar()` | Blocking single-byte read for the calling task (cooperatively yields). |
| `keyboard_poll()` | Non-blocking single-byte read; returns `0` if queue is empty. |
| `keyboard_set_focus(task)` | Set the task that receives input. `NULL` routes to the global ring. |
| `keyboard_send_to(task, c)` | Inject a byte directly into a task's ring (used by `vtty_switch` for `KEY_FOCUS_GAIN`). |
| `keyboard_release_task(task)` | Free a task's slot and clear focus/pane bindings on exit. |
| `keyboard_bind_pane(pane, task)` | Bind a task to `KB_PANE_TOP` / `KB_PANE_BOTTOM` for `Ctrl-A,U` / `Ctrl-A,J`. |
| `keyboard_focus_pane(pane)` | Move focus to the task bound to `pane`. |
| `keyboard_sigint_consume()` | Atomic test-and-clear of the SIGINT flag (returns `1` exactly once per `Ctrl+C`). |

The public API still uses `char` so existing consumers (`if (c == KEY_ARROW_UP) ...`)
continue to compile unchanged. The producer pipeline is `unsigned char` end
to end - see "Sentinel safety" below.

### Sentinel byte values (in `keyboard.h`)

| Sentinel | Byte | Notes |
|---|---|---|
| `KEY_ARROW_UP/DOWN/LEFT/RIGHT` | 0x80–0x83 | Outside `Ctrl+letter` range (0x01–0x1A) |
| `KEY_F1..F4` | 0x84–0x87 | Reserved; `Alt+F1..F4` is intercepted before delivery |
| `KEY_FOCUS_GAIN` | 0x88 | Sent by `vtty_switch` to the newly-focused task |
| `KEY_CTRL_C` | 0x03 | Plain ASCII ETX; delivered alongside `keyboard_sigint_consume` |

---

## Decoder state machine

The decoder is a four-state Mealy machine driven by raw PS/2 scancode set 1
bytes:

| State | Input | Action |
|---|---|---|
| any | `0xE0` | → `DEC_AFTER_E0` |
| any | `0xE1` | → `DEC_AFTER_E1A` |
| any | `0x00 0xAA 0xEE 0xFA 0xFE 0xFF` | → `DEC_NORMAL` (controller status, not a keystroke) |
| `DEC_NORMAL` | `b` | emit `kc = b & 0x7F`, `is_break = b >> 7` |
| `DEC_AFTER_E0` | `b` | emit `kc = KC_EXT(b & 0x7F)`, `is_break = b >> 7`; drop fake-shift padding (PrintScreen) |
| `DEC_AFTER_E1A` | `b` | → `DEC_AFTER_E1B` (consume first half of Pause) |
| `DEC_AFTER_E1B` | `b` | → `DEC_NORMAL` (consume second half of Pause) |

The state machine cannot livelock: every input either advances or terminates
the current sequence, and an unexpected `0xE0`/`0xE1` cleanly restarts the
prefix rather than poisoning the next byte. This is the single most
important fix relative to the previous driver, where a single "extended"
flag was set on `0xE0` and a lost byte could leave it sticky indefinitely.

### PrintScreen "fake shifts"

PrintScreen press/release sends `e0 2a e0 37` / `e0 b7 e0 aa`. The
`2a`/`aa` bytes are a backwards-compatibility hack from the original AT
keyboard and would, if interpreted literally, toggle real shift state on
every PrintScreen. We detect them in `DEC_AFTER_E0` (post-prefix value
== `KC_LSHIFT`) and drop them silently.

---

## Modifier tracking

Left and right modifiers are tracked independently:

```
mod_lshift, mod_rshift  → mod_shift = lshift | rshift
mod_lctrl,  mod_rctrl   → mod_ctrl  = lctrl  | rctrl
mod_lalt,   mod_ralt    → mod_alt   = lalt   | ralt
mod_caps                 (toggled on press only)
```

This avoids the classic bug where holding LShift, pressing and releasing
RShift, and continuing to hold LShift causes shift to silently turn off.

The compound `mod_*` fields are recomputed after every press/release event,
so `translate_make` only ever has to read one flag per modifier.

---

## SPSC ring buffers

Each task gets a 64-byte ring (`kb_slots[i].buf`); the global fallback ring
is 256 bytes. Both rings use `uint8_t` head/tail counters and a power-of-two
size so wraparound is exact: `(head - tail) mod 256` gives the occupancy.

### Memory ordering protocol

```
Producer (IRQ side):                 Consumer (task side):
   write data[head & MASK]              load head
   smp_wmb()        ◄── pairs ──►       smp_rmb()
   publish head++                       read data[tail & MASK]
                                        smp_wmb()
                                        publish tail++
```

On x86 / x86_64 TSO, `smp_wmb()` and `smp_rmb()` are compiler-only barriers
(no fence instructions emitted). On a hypothetical weakly-ordered port they
would expand to `dmb ishst` / `dmb ishld` or equivalents. Modeled on Linux's
`<asm/barrier.h>` so a future port can swap in arch-specific barriers
without touching the driver logic.

`READ_ONCE` / `WRITE_ONCE` (modeled on Linux's `<linux/compiler.h>`) are
used for any field read or written across context boundaries, to defeat
compiler hoisting and tear-introducing optimisations.

### Slot table - lock-free lookup, locked CAS mutation

`kb_slots[KB_TASK_SLOTS]` is a fixed array. `owner == NULL` means the slot
is free. Slots are **never compacted** - once a task is registered, its
slot index is stable for the task's lifetime.

- **Lookup** (`slot_lookup`): walks the array with `__atomic_load_n` on
  `owner`. Lock-free, safe from any context including IRQ.
- **Registration** (`slot_register`): fast path is a lock-free lookup;
  slow path takes `kb_slots_lock` and does a CAS-based claim of the first
  `NULL` slot. SMP-safe - concurrent registrations resolve via CAS.
- **Release** (`keyboard_release_task`): clears focus and pane bindings
  *before* nulling the owner pointer, so the IRQ never routes a byte to a
  released slot.

This is the same shape as Linux's many fixed-table-with-RCU-lookup
patterns, simplified for our environment where we don't yet have RCU.

---

## SMP readiness

Makar is currently UP, but the design is correct under SMP and will not
require revisits when a second CPU comes online:

- All cross-context shared state uses `__atomic_*` builtins or `READ_ONCE`/
  `WRITE_ONCE`. There are no naked accesses to a field that's modified by
  another context.
- Two spinlocks serialise mutation paths:
  - `kb_io_lock` around the controller drain (so two CPUs can't tear the
    PS/2 byte stream).
  - `kb_slots_lock` around slot registration/release (so two CPUs can't
    race-claim the same slot).
- Both spinlocks are IRQ-safe (`pushfl; cli` on acquire; `popfl` on
  release), so a CPU holding the lock cannot deadlock against its own IRQ
  handler trying to take the same lock.
- `kb_focused` and `kb_pane[]` are pointer-sized, naturally aligned, and
  always accessed via `__atomic_load_n` / `__atomic_store_n` with
  acquire/release semantics - so a future SMP runtime always sees a
  fully-published owner pointer for the current focus.

On UP, the spinlocks reduce to a single CAS on the fast path and an
unconditional store on release - i.e. effectively free.

---

## Sentinel safety (signed-char hazard)

The `KEY_*` sentinels live in `0x80..0x88`. Stored as a *signed* `char`
they sign-extend to `0xFFFFFF80..` when widened to `int`, which breaks any
comparison that goes via `int` - e.g. `c >= 0x80` evaluates `(int)(char)0x80
>= (int)0x80`, i.e. `-128 >= 128`, i.e. false.

Equality testing between two `char`-typed operands survives the widening
because both sides see the same bit pattern, which is why
`if (c == KEY_ARROW_UP) ...` continues to work in the shell. But the moment
a value is widened (`putchar(c)`, `printf("%d", c)`, `c >= 0x20`) the
sentinel is corrupted.

The driver enforces this discipline by:

1. Keeping the **producer pipeline `unsigned char` end-to-end** -
   scancodes, keycodes, ring storage, translator output.
2. Performing the conversion to `char` at exactly two places:
   `keyboard_getchar()` and `keyboard_poll()`, on the return value.
3. Documenting the sentinel byte values so consumers can decide whether to
   widen or not.

---

## Controller hygiene

On every IRQ:

1. Read `0x64` status; bail if `OBF` is clear.
2. Read `0x60` data - this acks the byte to the controller.
3. If `AUXB` is set the byte is from the mouse channel; discard.
4. Filter controller-internal status bytes (`0x00`, `0xAA`, `0xEE`, `0xFA`,
   `0xFE`, `0xFF`) - they are responses to commands, never keystrokes.
5. Loop up to 16 times so a runaway controller can't livelock the kernel,
   but stacked-up bytes from a previously-lost IRQ are still drained.

`keyboard_init()` also drains any leftover bytes the controller may have
queued before our handler was registered.

---

## Why the rewrite

Three classes of latent bug in the previous driver:

1. **Sticky `e0`** - `extended_key` was a single bit set on every `0xE0`
   byte and cleared only by the next non-prefix byte. A lost byte (typematic
   burst, lost EOI, emulator hiccup) left it sticky and silently corrupted
   the *next* normal scancode. New decoder is a full state machine and
   re-issuing `0xE0` cleanly restarts the prefix.

2. **SPSC ring race** - IRQ producer wrote `slot->buf[head]` then incremented
   `head` with no barrier between the two stores. Under `-O2` the compiler
   was free to reorder, and a consumer that observed the new `head` before
   the new byte landed would read stale ring memory. Most likely cause of
   the "sporadic single-character noise not correlated to keystrokes" symptom
   reported on PR #123.

3. **Slot-table tearing** - `kb_find_or_register()` mutated the slot array
   and `kb_nslots` from task context with no synchronisation. An IRQ1
   firing mid-mutation walked a partial table and either dereferenced
   garbage or routed to the wrong task. New design is fixed-slot + CAS, and
   the IRQ side never observes a partial state.

---

## Source map

| File | Role |
|---|---|
| `src/kernel/include/kernel/keyboard.h` | Public API, sentinel byte definitions |
| `src/kernel/arch/i386/drivers/keyboard.c` | Driver implementation (this document) |
| `src/kernel/arch/i386/proc/vtty.c` | Calls `keyboard_set_focus` / `keyboard_send_to` for TTY switching |
| `src/kernel/arch/i386/shell/shell.c` | Consumes `keyboard_getchar`; observes arrow / focus / Ctrl-C sentinels |
| `src/kernel/arch/i386/proc/vix.c` | Editor; consumes arrow sentinels |
| `src/kernel/arch/i386/shell/shell_cmd_apps.c` | Uses `keyboard_sigint_consume` to force-kill children during `exec` |

---

## Future work

- **Right modifier sentinels.** `RAlt` (AltGr) and `RCtrl` are tracked in
  the modifier state but no key currently distinguishes them from their
  left counterparts at the translator layer. Add when an international
  layout / dead-key support lands.
- **Home / End / PgUp / PgDn / Insert / Delete sentinels.** The keycodes
  are decoded but `translate_make` drops them today.
- **NumLock / ScrollLock LEDs.** Requires a controller write path
  (`keyboard_send_command`) and a small command queue.
- **Proper signal subsystem (slice #8).** Today `kb_sigint` is a single
  global; once `task->sig_pending` lands, Ctrl-C should target
  `kb_focused->sig_pending` directly.
- **Real per-task fd table (slice #7).** Stdin will become a regular fd
  pointing at the task's keyboard slot, removing the `keyboard_*`
  references from `syscall.c`'s read path.
