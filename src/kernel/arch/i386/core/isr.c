//
// isr.c -- High level interrupt service routines and interrupt request handlers.
// Part of this code is modified from Bran's kernel development tutorials.
// Rewritten for JamesM's kernel development tutorials.
//

#include <kernel/system.h>
#include <kernel/isr.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <kernel/signal.h>
#include <kernel/asm.h>

isr_t interrupt_handlers[256];

void init_isr_handlers()
{
	memset(&interrupt_handlers, 0, sizeof(isr_t)*256);
}

void register_interrupt_handler(uint8_t n, isr_t handler)
{
	interrupt_handlers[n] = handler;
	if (n >= IRQ0)
	{
		t_writestring("IRQ ");
		t_dec(n - IRQ0);
		KLOG("register_interrupt_handler: IRQ ");
		KLOG_DEC(n - IRQ0);
	}
	else
	{
		t_writestring("Interrupt ");
		t_dec(n);
		KLOG("register_interrupt_handler: INT ");
		KLOG_DEC(n);
	}
	t_writestring(" Registered...\n");
	KLOG(" registered\n");
}

void unregister_interrupt_handler(uint8_t n)
{
	t_writestring("Unregistering a handler...\n");
	KLOG("unregister_interrupt_handler: INT ");
	KLOG_DEC(n);
	KLOG("\n");
	interrupt_handlers[n] = 0x0;
}

int is_registered(uint8_t n)
{
	return !(interrupt_handlers[n] == 0);
}

// This gets called from our ASM interrupt handler stub.
void isr_handler(registers_t *regs)
{
	if(interrupt_handlers[regs->int_no] != 0)
	{
		isr_t handler = interrupt_handlers[regs->int_no];
		handler(regs);
	}
	else
	{
		t_writestring("Unhandled Interrupt: ");
		t_dec(regs->int_no);
		t_putchar('\n');
		PANIC("Unhandled Interrupt");
	}

	/* Slice 8 phase 4: if we're about to iret back to ring 3, deliver
	 * any pending user-handler signal via the trampoline route.  No-op
	 * for ring-0 frames or when no user handler is installed. */
	signal_check_user(regs);
}

// This gets called from our ASM interrupt handler stub.
void irq_handler(registers_t *regs)
{
	if(interrupt_handlers[regs->int_no] != 0)
	{
		isr_t handler = interrupt_handlers[regs->int_no];
		handler(regs);
	}

	// Send an EOI (end of interrupt) signal to the PICS.
	// If this interrupt involved the slave
	if (regs->int_no >= 40)
	{
		// Send reset signal to slave.
		outb(0xA0, 0x20);
	}

	// Send reset signal to master. (As well as slave, if necessary).
	outb(0x20, 0x20);

	/* Slice 8 phase 4: same hook as isr_handler -- a timer IRQ that
	 * preempted ring 3 is the most common path here. */
	signal_check_user(regs);
}