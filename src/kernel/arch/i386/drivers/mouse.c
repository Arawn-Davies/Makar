/*
 * mouse.c -- PS/2 mouse driver (IRQ12).
 *
 * Shares the 8042 controller with the keyboard.  The keyboard IRQ1 handler
 * drains the controller greedily and hands any AUX-channel byte to
 * mouse_feed_byte(); our IRQ12 handler does the symmetric thing.  Either path
 * routes a byte by AUXB so no packet is lost regardless of which line fired.
 *
 * Events are queued in a single-producer/single-consumer ring (producer = IRQ
 * context, consumer = SYS_MOUSE_READ) and drained by mouse_pop_event().
 *
 * Bring-up only -- UP, polled command/ACK handshake at init.  See
 * docs/plans/gui-wm.md (slice G1).
 */

#include <kernel/mouse.h>
#include <kernel/isr.h>
#include <kernel/asm.h>
#include <kernel/vm.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_OBF    0x01   /* output buffer full (byte ready in 0x60) */
#define PS2_IBF    0x02   /* input  buffer full (host write pending) */
#define PS2_AUXB   0x20   /* byte came from the AUX (mouse) channel  */

/* Event ring (SPSC). Power-of-two size; head advanced by IRQ, tail by reader. */
#define MOUSE_RING 64
static volatile uint32_t s_ring[MOUSE_RING];
static volatile uint32_t s_head = 0;   /* producer */
static volatile uint32_t s_tail = 0;   /* consumer */

/* Packet assembler state. */
static uint8_t  s_pkt[3];
static int      s_idx = 0;
static int      s_ready = 0;   /* gate: ignore bytes until init completes */

/* ---- controller helpers (polled, bounded) ------------------------------- */

static void ps2_wait_input(void)   /* wait until host can write (IBF clear) */
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(PS2_STATUS) & PS2_IBF)) return;
}

static void ps2_wait_output(void)  /* wait until a byte is readable (OBF set) */
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & PS2_OBF) return;
}

/* Write one byte to the mouse (0xD4 prefix), then read+discard its ACK. */
static void mouse_cmd(uint8_t b)
{
    ps2_wait_input(); outb(PS2_CMD, 0xD4);
    ps2_wait_input(); outb(PS2_DATA, b);
    ps2_wait_output(); (void)inb(PS2_DATA);   /* 0xFA ACK */
}

/* ---- device-independent event sink -------------------------------------- */

/*
 * mouse_post_event - queue one decoded motion/button event.  This is the input
 * layer's device-independent entry point: the PS/2 packet assembler below feeds
 * it, and a USB HID boot-mouse driver feeds it the same way, so the mouse is no
 * longer tied to PS/2.  dx/+x is right, dy/+y is screen-down, buttons bit0=left
 * bit1=right bit2=middle.  Safe to call from IRQ context (SPSC ring).
 */
void mouse_post_event(int dx, int dy, int buttons)
{
    if (dx >  127) dx =  127; else if (dx < -127) dx = -127;
    if (dy >  127) dy =  127; else if (dy < -127) dy = -127;

    uint32_t ev = (1u << 31)
                | (uint32_t)(buttons & 0x07)
                | ((uint32_t)(dx & 0xFF) << 8)
                | ((uint32_t)(dy & 0xFF) << 16);

    uint32_t next = (s_head + 1) % MOUSE_RING;
    if (next != s_tail) {       /* drop on full */
        s_ring[s_head] = ev;
        s_head = next;
    }
}

/* ---- PS/2 packet assembly ----------------------------------------------- */

void mouse_feed_byte(uint8_t b)
{
    if (!s_ready) return;

    /* Resync: byte 0 must have bit 3 set. */
    if (s_idx == 0 && !(b & 0x08)) return;

    s_pkt[s_idx++] = b;
    if (s_idx < 3) return;
    s_idx = 0;

    uint8_t flags = s_pkt[0];
    if (flags & 0xC0) return;   /* X/Y overflow -- drop */

    int dx = (int)s_pkt[1] - ((flags & 0x10) ? 256 : 0);
    int dy = (int)s_pkt[2] - ((flags & 0x20) ? 256 : 0);
    /* Standard PS/2 reports +y up, so negate to screen +y down.  Hyper-V's
     * emulated mouse reports the opposite convention -- negating there inverts
     * it -- so skip the flip under Hyper-V.  (Reasoned from the inverted-Y
     * report; gated by vm_kind() so QEMU/Bochs/VBox/VMware are unaffected.) */
    if (vm_kind() != VM_HYPERV)
        dy = -dy;

    mouse_post_event(dx, dy, flags & 0x07);
}

uint32_t mouse_pop_event(void)
{
    if (s_tail == s_head) return 0;
    uint32_t ev = s_ring[s_tail];
    s_tail = (s_tail + 1) % MOUSE_RING;
    return ev;
}

void mouse_inject_packet(uint8_t b0, uint8_t b1, uint8_t b2)
{
    int saved = s_ready; s_ready = 1; s_idx = 0;
    mouse_feed_byte(b0); mouse_feed_byte(b1); mouse_feed_byte(b2);
    s_ready = saved;
}

/* ---- IRQ + init --------------------------------------------------------- */

static void mouse_irq_handler(registers_t *regs)
{
    (void)regs;
    /* Take only AUX (mouse) bytes; a keyboard byte is left for IRQ1 (reading it
     * here would steal keystrokes -- the keyboard-injection test relies on this).
     * NOTE: on a strict 8042 (VirtualBox/VMware) a left-behind byte can stall the
     * mouse; the robust path there is a USB HID mouse, not fighting the 8042. */
    for (int i = 0; i < 16; i++) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & PS2_OBF)) break;
        if (!(status & PS2_AUXB)) break;
        mouse_feed_byte(inb(PS2_DATA));
    }
}

void mouse_init(void)
{
    /* Enable the aux device. */
    ps2_wait_input(); outb(PS2_CMD, 0xA8);

    /* Read controller config, enable IRQ12 (bit1), clear aux-clock-disable
     * (bit5), write it back. */
    ps2_wait_input(); outb(PS2_CMD, 0x20);
    ps2_wait_output(); uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;
    cfg &= ~0x20;
    ps2_wait_input(); outb(PS2_CMD, 0x60);
    ps2_wait_input(); outb(PS2_DATA, cfg);

    /* Defaults + enable data reporting.  ACKs drained synchronously while the
     * handler is not yet registered, so 0xFA never enters the packet stream. */
    mouse_cmd(0xF6);   /* set defaults */
    mouse_cmd(0xF4);   /* enable reporting */

    /* Drain anything left in OBF before we go live. */
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS) & PS2_OBF)) break;
        (void)inb(PS2_DATA);
    }

    s_idx = 0;
    s_ready = 1;
    register_interrupt_handler(IRQ12, mouse_irq_handler);
}
