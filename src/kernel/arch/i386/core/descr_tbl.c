#include <kernel/asm.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <stdio.h>
#include <string.h>
#include <kernel/descr_tbl.h>
#include <kernel/isr.h>

/* OSDev references:
 *   GDT / segment descriptors  – https://wiki.osdev.org/GDT
 *   GDT Tutorial (flush + far jump) – https://wiki.osdev.org/GDT_Tutorial
 *   IDT / interrupt gate descriptors – https://wiki.osdev.org/IDT
 *   8259 PIC remapping            – https://wiki.osdev.org/8259_PIC
 *   Task State Segment (TSS)      – https://wiki.osdev.org/TSS
 */

/* Lets us access our ASM functions from our C code. */
extern void gdt_flush(uint32_t);
extern void idt_flush(uint32_t);
extern void tss_flush(void);

/* Internal function prototypes. */
static void init_gdt();
static void gdt_set_gate(int32_t, uint32_t, uint32_t, uint8_t, uint8_t);
static void tss_set_gate(int32_t num, uint32_t base, uint32_t limit);

static void init_idt();
static void idt_set_gate(uint8_t, uint32_t, uint16_t, uint8_t);

/* GDT now has 6 entries: null, kernel code, kernel data,
   user code, user data, TSS. */
gdt_entry_t	gdt_entries[6];
gdt_ptr_t	gdt_ptr;
idt_entry_t	idt_entries[256];
idt_ptr_t	idt_ptr;

/* The single TSS for the system.  ESP0/SS0 are updated per task-switch. */
static tss_t tss;

/* Initialisation routine – zeroes all the interrupt service routines,
 * initialises the GDT and IDT.
 * See: https://wiki.osdev.org/GDT_Tutorial, https://wiki.osdev.org/IDT
 */
void init_descriptor_tables()
{
	/* Initialise the global descriptor table. */
	init_gdt();
	t_writestring("GDT Initialised.\n");
	KLOG("init_descriptor_tables: GDT OK\n");
	init_idt();
	t_writestring("IDT Initialised.\n");
	KLOG("init_descriptor_tables: IDT OK\n");
	init_isr_handlers();
	t_writestring("ISR Handlers Initialised.\n");
	KLOG("init_descriptor_tables: ISR handlers OK\n");
}

/*
 * init_idt – populate all 256 IDT gate descriptors and load the IDT.
 *
 * Before installing gates we remap the 8259 PIC so that hardware IRQs
 * (IRQ 0–15) are delivered on vectors 32–47 instead of the default 8–15
 * (which conflicts with CPU exceptions).  See https://wiki.osdev.org/8259_PIC
 * for the full ICW1–ICW4 initialisation sequence used here.
 *
 * Each idt_set_gate() call uses selector 0x08 (kernel code segment) and
 * flags 0x8E (interrupt gate, DPL=0, present).  The syscall gate at
 * vector 128 uses 0xEE (DPL=3) so that user-mode code can invoke int 0x80.
 * See https://wiki.osdev.org/IDT for the gate-descriptor bit layout.
 */
static void init_idt()
{
	idt_ptr.limit = sizeof(idt_entry_t) * 256 -1;
	idt_ptr.base = (uint32_t)&idt_entries;

	memset(&idt_entries, 0, sizeof(idt_entry_t)*256);

	/*
	 * Remap the 8259 PIC (master + slave) via ICW1–ICW4 so that hardware
	 * IRQs land on vectors 32–47 rather than the default 8–23.
	 *   Master PIC: command port 0x20, data port 0x21
	 *   Slave  PIC: command port 0xA0, data port 0xA1
	 * ICW1 (0x11): start initialisation, cascade mode, ICW4 needed.
	 * ICW2: master offset 0x20 (32), slave offset 0x28 (40).
	 * ICW3: master pin 2 has slave (0x04), slave ID is 2 (0x02).
	 * ICW4 (0x01): 8086/88 mode.
	 * OCW1: unmask all IRQs (0x00 for both PICs).
	 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x20);
	outb(0xA1, 0x28);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0x0);
	outb(0xA1, 0x0);

	idt_set_gate( 0, (uint32_t)isr0 , 0x08, 0x8E);
        idt_set_gate( 1, (uint32_t)isr1 , 0x08, 0x8E);
        idt_set_gate( 2, (uint32_t)isr2 , 0x08, 0x8E);
        idt_set_gate( 3, (uint32_t)isr3 , 0x08, 0x8E);
        idt_set_gate( 4, (uint32_t)isr4 , 0x08, 0x8E);
        idt_set_gate( 5, (uint32_t)isr5 , 0x08, 0x8E);
        idt_set_gate( 6, (uint32_t)isr6 , 0x08, 0x8E);
        idt_set_gate( 7, (uint32_t)isr7 , 0x08, 0x8E);
        idt_set_gate( 8, (uint32_t)isr8 , 0x08, 0x8E);
        idt_set_gate( 9, (uint32_t)isr9 , 0x08, 0x8E);
        idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
        idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
        idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
        idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
        idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
        idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
        idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
        idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
        idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
        idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
        idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
        idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
        idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
        idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
        idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
        idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
        idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
        idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
        idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
        idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
        idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
	idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

        idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
        idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
        idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);
        idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);
        idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);
        idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);
        idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);
        idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);
        idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);
        idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);
        idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
        idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
        idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
        idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
        idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
        idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

	/* Syscall gate: int 0x80 (vector 128).
	 * DPL=3 (0xEE) so user-mode code can invoke it. */
	idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE);

	idt_flush((uint32_t)&idt_ptr);
}

/*
 * init_gdt – install 6 GDT descriptors and reload all segment registers.
 *
 * Flat 4 GiB segments (base=0, limit=0xFFFFFFFF) are used for both code
 * and data in ring 0 and ring 3.  Segmentation is effectively disabled;
 * all protection is handled by the paging unit.
 * See https://wiki.osdev.org/GDT and https://wiki.osdev.org/GDT_Tutorial
 */
static void init_gdt()
{
	gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
	gdt_ptr.base = (uint32_t)&gdt_entries;

	gdt_set_gate(0, 0, 0, 0, 0);                       /* Null segment           */
	gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);        /* Kernel code  (0x08)    */
	gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);        /* Kernel data  (0x10)    */
	gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);        /* User code    (0x18)    */
	gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);        /* User data    (0x20)    */
	tss_set_gate(5, (uint32_t)&tss, sizeof(tss) - 1);  /* TSS          (0x28)    */

	gdt_flush((uint32_t)&gdt_ptr);
	tss_flush();
}

/* idt_set_gate – write one 8-byte interrupt-gate descriptor into the IDT.
 * `base` is the 32-bit handler address; `sel` is the code segment (0x08);
 * `flags` encodes gate type (0x8E = interrupt gate, DPL=0, present).
 * See https://wiki.osdev.org/IDT for the full bit layout.
 */
static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
	idt_entries[num].base_lo = base & 0xFFFF;
	idt_entries[num].base_hi = (base >> 16) & 0xFFFF;

	idt_entries[num].sel		= sel;
	idt_entries[num].always0	= 0;
	idt_entries[num].flags		= flags;
}

/* gdt_set_gate – write one 8-byte segment descriptor into the GDT.
 * `access` encodes type, DPL and present bit; `gran` encodes the G/D/L/AVL
 * nibble that is combined with the top 4 bits of the limit.
 * See https://wiki.osdev.org/GDT for the full bit layout.
 */
static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
	gdt_entries[num].base_low	= (base & 0xFFFF);
	gdt_entries[num].base_middle	= (base >> 16) & 0xFF;
	gdt_entries[num].base_high	= (base >> 24) & 0xFF;

	gdt_entries[num].limit_low	= (limit & 0xFFFF);
	gdt_entries[num].granularity	= (limit >> 16) & 0x0F;

	gdt_entries[num].granularity	|= gran & 0xF0;
	gdt_entries[num].access		= access;
}

/*
 * tss_set_gate – write a TSS descriptor into a GDT slot.
 *
 * The TSS descriptor is a "system" descriptor (S bit clear).  For a
 * 32-bit available TSS the access byte is 0x89 (P=1, DPL=0, type=0x9).
 * Granularity is 0x00 (byte granularity; the TSS is only a few hundred
 * bytes so we don't need 4 KiB granularity).
 */
static void tss_set_gate(int32_t num, uint32_t base, uint32_t limit)
{
	/* Zero the TSS itself before installing. */
	memset(&tss, 0, sizeof(tss));

	/* Kernel stack segment; ESP0 filled in by tss_set_kernel_stack(). */
	tss.ss0       = 0x10;   /* kernel data segment */
	tss.esp0      = 0;      /* updated before first Ring-3 entry   */
	tss.iomap_base = sizeof(tss); /* no I/O permission bitmap */

	/* Write the GDT descriptor for the TSS. */
	gdt_entries[num].limit_low   = (uint16_t)(limit & 0xFFFF);
	gdt_entries[num].base_low    = (uint16_t)(base  & 0xFFFF);
	gdt_entries[num].base_middle = (uint8_t)((base  >> 16) & 0xFF);
	gdt_entries[num].access      = 0x89;  /* P=1, DPL=0, 32-bit TSS available */
	gdt_entries[num].granularity = (uint8_t)(((limit >> 16) & 0x0F));
	gdt_entries[num].base_high   = (uint8_t)((base  >> 24) & 0xFF);
}

/*
 * tss_set_kernel_stack – update the kernel-stack pointer in the TSS.
 *
 * Must be called before every Ring-3 → Ring-0 transition to ensure the
 * CPU switches to the correct per-task kernel stack.
 */
void tss_set_kernel_stack(uint32_t esp0)
{
	tss.esp0 = esp0;
}

uint32_t tss_get_esp0(void)
{
	return tss.esp0;
}