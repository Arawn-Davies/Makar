#ifndef _KERNEL_MOUSE_H
#define _KERNEL_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

/* PS/2 mouse driver (IRQ12).  Bring-up companion to the keyboard driver;
 * shares the 8042 controller.  Events are queued as packed uint32 values and
 * drained by SYS_MOUSE_READ.  See docs/plans/gui-wm.md (slice G1). */

/* Enable the aux device + IRQ12, set sane defaults, register the handler. */
void mouse_init(void);

/* Feed one controller byte into the packet assembler.  Called from the IRQ12
 * handler and from the keyboard IRQ1 handler when it drains an AUX byte. */
void mouse_feed_byte(uint8_t b);

/* Device-independent event sink: queue one decoded motion/button event.  Fed by
 * the PS/2 packet assembler and by any other pointer source (USB HID boot mouse)
 * so the mouse is not tied to PS/2.  +x right, +y screen-down; buttons bit0=left
 * bit1=right bit2=middle.  IRQ-safe. */
void mouse_post_event(int dx, int dy, int buttons);

/* Pop one event.  Returns 0 when the queue is empty, else a packed value:
 *   bit  31      always 1 (valid marker, so a zero-motion event isn't 0)
 *   bits  0..2   buttons: bit0 left, bit1 right, bit2 middle
 *   bits  8..15  dx  (signed 8-bit, +x right)
 *   bits 16..23  dy  (signed 8-bit, +y down -- already screen-oriented)
 */
uint32_t mouse_pop_event(void);

/* Test hook: inject a raw 3-byte PS/2 packet as if from hardware. */
void mouse_inject_packet(uint8_t b0, uint8_t b1, uint8_t b2);

/* Format a text snapshot of the input chain (irq12 -> bytes -> packets ->
 * events -> position) into buf.  Backs /dev/mouse for diagnosing dead pointers.
 * Returns the byte count written (excluding the NUL terminator). */
int mouse_render_stats(char *buf, int cap);

#endif
