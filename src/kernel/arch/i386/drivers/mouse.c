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
#include <kernel/i8042.h>
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

/* ---- diagnostics (snapshot via /dev/mouse, mouse_render_stats) -----------
 * Counters along the chain IRQ12 -> AUX bytes -> packets -> events -> motion,
 * so a probe can see exactly where input stops (e.g. on a hypervisor where the
 * mouse appears dead).  Producer is IRQ context; readers only snapshot. */
static volatile uint32_t s_irq12;       /* IRQ12 service routine invocations  */
static volatile uint32_t s_bytes;       /* AUX bytes handed to mouse_feed_byte */
static volatile uint32_t s_packets;     /* complete 3-byte packets assembled   */
static volatile uint32_t s_resync;      /* byte-0 sync-bit failures dropped     */
static volatile uint32_t s_overflow;    /* X/Y overflow packets dropped         */
static volatile uint32_t s_events;      /* events posted to the ring            */
static volatile int      s_acc_x, s_acc_y;     /* accumulated cursor position   */
static volatile int      s_last_dx, s_last_dy; /* last decoded motion           */
static volatile uint8_t  s_buttons;            /* last button bitmask           */
static volatile uint8_t  s_last_pkt[3];        /* last raw 3-byte packet        */

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

    s_events++;
    s_last_dx = dx; s_last_dy = dy; s_buttons = (uint8_t)(buttons & 0x07);
    s_acc_x += dx; if (s_acc_x < 0) s_acc_x = 0; else if (s_acc_x > 4095) s_acc_x = 4095;
    s_acc_y += dy; if (s_acc_y < 0) s_acc_y = 0; else if (s_acc_y > 4095) s_acc_y = 4095;

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
    s_bytes++;

    /* Resync: byte 0 must have bit 3 set. */
    if (s_idx == 0 && !(b & 0x08)) { s_resync++; return; }

    s_pkt[s_idx++] = b;
    if (s_idx < 3) return;
    s_idx = 0;
    s_packets++;
    s_last_pkt[0] = s_pkt[0]; s_last_pkt[1] = s_pkt[1]; s_last_pkt[2] = s_pkt[2];

    uint8_t flags = s_pkt[0];
    if (flags & 0xC0) { s_overflow++; return; }   /* X/Y overflow -- drop */

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

/* ---- /dev/mouse snapshot ------------------------------------------------- */

static int ms_puts(char *b, int cap, int n, const char *s)
{ while (*s && n < cap - 1) b[n++] = *s++; return n; }
static int ms_putu(char *b, int cap, int n, uint32_t v)
{ char t[12]; int k = 0; if (!v) { if (n < cap-1) b[n++] = '0'; return n; }
  while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
  while (k && n < cap-1) b[n++] = t[--k]; return n; }
static int ms_puti(char *b, int cap, int n, int v)
{ if (v < 0) { if (n < cap-1) b[n++] = '-'; v = -v; } return ms_putu(b, cap, n, (uint32_t)v); }
static int ms_puthex2(char *b, int cap, int n, uint8_t v)
{ const char *h = "0123456789abcdef";
  if (n < cap-1) b[n++] = h[(v >> 4) & 0xf];
  if (n < cap-1) b[n++] = h[v & 0xf]; return n; }

/*
 * mouse_render_stats - format a text snapshot of the input chain into buf,
 * NUL-terminated.  Backs /dev/mouse so `cat /dev/mouse` (or the mouse probe)
 * shows where input breaks: irq12 -> bytes -> packets -> events -> position.
 * On a hypervisor where the cursor is dead, the first zero counter pinpoints
 * the failing stage (no IRQ12, or IRQ12 but no AUX bytes, or no sync, ...).
 * Returns the byte count (excluding the NUL).
 */
int mouse_render_stats(char *buf, int cap)
{
    if (!buf || cap <= 1) { if (buf && cap > 0) buf[0] = '\0'; return 0; }
    int n = 0;
    n = ms_puts(buf, cap, n, "mouse: ready=");   n = ms_putu(buf, cap, n, (uint32_t)s_ready);
    n = ms_puts(buf, cap, n, " vm=");            n = ms_putu(buf, cap, n, (uint32_t)vm_kind());
    n = ms_puts(buf, cap, n, "\nirq12=");        n = ms_putu(buf, cap, n, s_irq12);
    n = ms_puts(buf, cap, n, " bytes=");         n = ms_putu(buf, cap, n, s_bytes);
    n = ms_puts(buf, cap, n, " packets=");       n = ms_putu(buf, cap, n, s_packets);
    n = ms_puts(buf, cap, n, " resync=");        n = ms_putu(buf, cap, n, s_resync);
    n = ms_puts(buf, cap, n, " overflow=");      n = ms_putu(buf, cap, n, s_overflow);
    n = ms_puts(buf, cap, n, "\nevents=");       n = ms_putu(buf, cap, n, s_events);
    n = ms_puts(buf, cap, n, " buttons=");
    n = ms_puts(buf, cap, n, (s_buttons & 1) ? "L" : "-");
    n = ms_puts(buf, cap, n, (s_buttons & 2) ? "R" : "-");
    n = ms_puts(buf, cap, n, (s_buttons & 4) ? "M" : "-");
    n = ms_puts(buf, cap, n, "\nlast_dx=");      n = ms_puti(buf, cap, n, s_last_dx);
    n = ms_puts(buf, cap, n, " last_dy=");       n = ms_puti(buf, cap, n, s_last_dy);
    n = ms_puts(buf, cap, n, " pos=(");          n = ms_puti(buf, cap, n, s_acc_x);
    n = ms_puts(buf, cap, n, ",");               n = ms_puti(buf, cap, n, s_acc_y);
    n = ms_puts(buf, cap, n, ")\nlast_pkt=");
    n = ms_puthex2(buf, cap, n, s_last_pkt[0]);  n = ms_puts(buf, cap, n, " ");
    n = ms_puthex2(buf, cap, n, s_last_pkt[1]);  n = ms_puts(buf, cap, n, " ");
    n = ms_puthex2(buf, cap, n, s_last_pkt[2]);
    n = ms_puts(buf, cap, n, "\n");
    buf[n] = '\0';
    return n;
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
    s_irq12++;
    /* IRQ12 (AUX/mouse) and the keyboard's IRQ1 both funnel into the shared
     * controller router, which drains the 8042 and dispatches each byte by its
     * AUXB bit.  Servicing the controller from whichever line fires means a byte
     * is never left in the output buffer -- a left-behind byte stops a strict
     * 8042 (VirtualBox/VMware) raising further IRQ12s, which froze the mouse
     * after a few packets.  The AUX-only handler this replaces is what left
     * keyboard bytes stuck. */
    i8042_service();
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
