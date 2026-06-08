/*
 * i8042.c -- shared PS/2 controller (8042) byte router.
 *
 * The 8042 multiplexes two devices onto one output buffer: the keyboard raises
 * IRQ1, the AUX channel (mouse) raises IRQ12.  Both IRQ handlers funnel into
 * i8042_service(), which drains the output buffer and routes each byte by its
 * AUXB status bit -- mouse bytes to the mouse packet assembler, everything else
 * to the keyboard decoder.
 *
 * Routing here (rather than in either device driver) means a byte is always
 * read and dispatched no matter which line fired, so none is ever left sitting
 * in the output buffer.  A left-behind byte stops a strict controller
 * (VirtualBox/VMware) from raising further IRQ12s, which froze the mouse after a
 * few packets.  It also keeps the keyboard and mouse drivers as independent
 * clients of the controller rather than reaching into each other.
 */
#include <kernel/i8042.h>
#include <kernel/keyboard.h>   /* keyboard_feed_scancode */
#include <kernel/mouse.h>      /* mouse_feed_byte        */
#include <kernel/asm.h>        /* inb                    */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_OBF     0x01   /* output buffer full -- byte ready in 0x60 */
#define PS2_AUXB    0x20   /* byte came from the AUX (mouse) channel    */

void i8042_service(void)
{
    /* Bounded drain.  Runs in IRQ context (IF clear) on a uniprocessor, so the
     * two IRQ lines can't interleave here; the keyboard decoder and mouse ring
     * are each serialised inside their own feed routine, so no controller-level
     * lock is needed.  The 16-byte cap bounds a runaway controller. */
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & PS2_OBF)) break;
        uint8_t b = inb(PS2_DATA);
        if (status & PS2_AUXB) mouse_feed_byte(b);
        else                   keyboard_feed_scancode(b);
    }
}
