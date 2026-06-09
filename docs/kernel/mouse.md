---
title: mouse
parent: Kernel subsystems
grand_parent: Reference
---

# mouse - PS/2 mouse driver (IRQ12) + device-independent event queue

**Header:** `kernel/include/kernel/mouse.h`
**Source:** `arch/i386/drivers/mouse.c`

The bring-up companion to the [keyboard](keyboard.md) driver. It shares the 8042
controller (the keyboard and mouse sit on the same chip; a shared i8042
controller module owns the port handshake so `mouse.c` does not depend on
`keyboard.c`). Decoded motion/button events are queued as packed `uint32`
values and drained by `SYS_MOUSE_READ`; the WM reads them to move its cursor.

The event sink is **device-independent** (`mouse_post_event`), so a future USB
HID boot mouse can feed the same queue — the GUI is not tied to PS/2.

---

## API

| Function | Role |
|---|---|
| `mouse_init()` | Enable the aux device + IRQ12, set sane defaults, register the handler. |
| `mouse_feed_byte(b)` | Feed one controller byte into the packet assembler. Called from the IRQ12 handler **and** from the keyboard IRQ1 handler when it drains an AUX byte. |
| `mouse_post_event(dx,dy,buttons)` | Device-independent sink: queue one decoded event. IRQ-safe. `+x` right, `+y` screen-down; buttons bit0=left, bit1=right, bit2=middle. |
| `mouse_pop_event()` | Pop one event; `0` when empty, else a packed value (below). |
| `mouse_inject_packet(b0,b1,b2)` | Test hook: inject a raw 3-byte PS/2 packet as if from hardware. |
| `mouse_render_stats(buf,cap)` | Text snapshot of the input chain (irq12 → bytes → packets → events → position); backs `/dev/mouse` and the `mouseprobe` diagnostic. |

## Packed event word (`mouse_pop_event`)

| Bits | Meaning |
|---|---|
| 31 | always 1 — valid marker, so a zero-motion event isn't `0` |
| 0..2 | buttons: bit0 left, bit1 right, bit2 middle |
| 8..15 | `dx` (signed 8-bit, +x right) |
| 16..23 | `dy` (signed 8-bit, +y down — already screen-oriented) |

---

## Notes

- **`+y` is screen-oriented** (down-positive) at the event boundary. PS/2
  reports up-positive Y; the driver flips it. Hyper-V's PS/2 emulation uses a
  different Y convention, applied as a [vm](vm.md)-keyed quirk.
- `/dev/mouse` exposes `mouse_render_stats()` for diagnosing a dead pointer —
  it shows where the chain breaks (no IRQ? bytes but no packets? events but no
  movement?). The userspace `mouseprobe` tool reads it.

See also: [keyboard](keyboard.md), [vm](vm.md), `docs/gui.md`.
