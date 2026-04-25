/*
 * timer.c — Intel 8253/8254 PIT driver.
 *
 * Programs PIT channel 0 to fire IRQ 0 at a configurable rate and provides
 * a busy-wait sleep built on the resulting tick counter.
 *
 * Reference: https://wiki.osdev.org/PIT
 *
 * PIT I/O ports:
 *   0x40 – Channel 0 data (read/write, byte-at-a-time).
 *   0x41 – Channel 1 data (unused here; historically DRAM refresh).
 *   0x42 – Channel 2 data (unused here; drives PC speaker).
 *   0x43 – Mode/Command register (write-only).
 *
 * The PIT's internal oscillator runs at 1 193 180 Hz (derived from the
 * original IBM PC 14.318 MHz crystal divided by 12).  Channel 0 counts down
 * from the 16-bit reload value and fires IRQ 0 each time it reaches zero.
 * To achieve a tick rate of F Hz, write the divisor 1193180 / F to port 0x40.
 */

#include <kernel/timer.h>
#include <kernel/isr.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <kernel/asm.h>

static volatile uint32_t tick = 0;

void timer_callback(registers_t *regs)
{
	(void)regs;
	tick++;
	t_spinner_tick(tick);
}

uint32_t timer_get_ticks(void)
{
	return tick;
}

void ksleep(uint32_t ticks)
{
	/* Note: wraps safely since both values are uint32_t and
	 * the comparison handles the common case where ticks is small. */
	uint32_t end = tick + ticks;
	while (tick < end)
		;
}

void init_timer(uint32_t frequency)
{
	/* Register the IRQ 0 callback before enabling the counter. */
	register_interrupt_handler(IRQ0, &timer_callback);

	/*
	 * Compute the 16-bit reload divisor.
	 *   PIT input clock: 1 193 180 Hz
	 *   Divisor = 1193180 / frequency
	 * The result must fit in 16 bits (max divisor = 65535 → min freq ≈ 18 Hz).
	 */
	uint32_t divisor = 1193180 / frequency;

	/*
	 * Write the command byte to port 0x43 (Mode/Command register).
	 * Byte format (bits 7–0):
	 *   7–6: channel select  = 00 (channel 0)
	 *   5–4: access mode     = 11 (lo byte then hi byte)
	 *   3–1: operating mode  = 011 (mode 3: square wave generator)
	 *   0:   BCD/binary      = 0  (16-bit binary)
	 * 0x36 = 0b 00 11 011 0
	 */
	outb(0x43, 0x36);

	/*
	 * Send the divisor to channel 0 (port 0x40), low byte first then high byte.
	 * The access mode "11" selected above requires exactly this order.
	 */
	uint8_t l = (uint8_t)(divisor & 0xFF);
	uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

	outb(0x40, l);
	outb(0x40, h);

	/*
	 * Multiboot 2 enters the kernel with interrupts disabled.  Enable them
	 * now that the IDT and PIT are fully configured so that IRQs can fire.
	 */
	enable_interrupts();
	KLOG("init_timer: ");
	KLOG_DEC(frequency);
	KLOG(" Hz, interrupts enabled\n");
}