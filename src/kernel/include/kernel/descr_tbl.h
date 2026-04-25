//
// ISR.h -- Interface and structures for high level interrupt service routines.
//          Part of this code is modified from Bran's kernel development tutorials.
//          Rewritten for JamesM's kernel development tutorials.
//

#ifndef _KERNEL_INTERRUPTS_H
#define _KERNEL_INTERRUPTS_H
#include <kernel/types.h>

// This structure contains the value of one GDT entry.
// We use the attribute 'packed' to tell GCC not to change
// any of the alignment in the structure.
struct gdt_entry_struct
{
	uint16_t	limit_low;	// The lower 16 bits of the limit.
	uint16_t	base_low;	// The lower 16 bits of the base.
	uint8_t	base_middle;	// The next 8 bits of the base.
	uint8_t	access;		// Access flags, determine what ring this segment can be used in.
	uint8_t	granularity;
	uint8_t	base_high;	// the last 8 bits of the base.
} __attribute__((packed));
typedef struct gdt_entry_struct gdt_entry_t;

struct gdt_ptr_struct
{
	uint16_t	limit;		// The upper 16 bits of all selector limits
	uint32_t	base;		// The address of the first gdt_entry_t struct
} __attribute__((packed));
typedef struct gdt_ptr_struct gdt_ptr_t;

/*
 * i386 Task State Segment (minimal hardware TSS).
 *
 * The CPU reads ESP0/SS0 from the TSS on every Ring-3 → Ring-0 transition
 * (int, irq, syscall) to know where to place the kernel stack.  Only these
 * two fields plus iomap_base need to be valid for our purposes.
 */
typedef struct tss_struct {
    uint32_t prev_tss;   /* previous TSS selector (unused, 0)          */
    uint32_t esp0;       /* kernel stack pointer for Ring-0 entry       */
    uint32_t ss0;        /* kernel stack segment  (0x10)                */
    uint32_t esp1;       /* unused                                      */
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base; /* offset to I/O permission bitmap (set > TSS size = no bitmap) */
} __attribute__((packed)) tss_t;

// A struct describing an interrupt gate.
struct idt_entry_struct
{
	uint16_t	base_lo;	// The lower 16 bits of the address to jump to when this interrupt fires.
	uint16_t	sel;		// Kernel segment selector.
	uint8_t	always0;	// This must always be zero.
	uint8_t	flags;		// More flags.  See documentation.
	uint16_t	base_hi;	// The upper 16 bits of the address to jump to.
} __attribute__((packed));
typedef struct idt_entry_struct idt_entry_t;

// A struct describing a pointer to an array of inerrupt handlers.
// This is in a format suitable for giving to 'lidt'.
struct idt_ptr_struct
{
	uint16_t	limit;
	uint32_t	base;		// The address of the first element in our idt_entry_t array.
} __attribute__((packed));
typedef struct idt_ptr_struct idt_ptr_t;

// These extern directives let us access the addresses of our ASM ISR handlers.
extern void isr0 ();
extern void isr1 ();
extern void isr2 ();
extern void isr3 ();
extern void isr4 ();
extern void isr5 ();
extern void isr6 ();
extern void isr7 ();
extern void isr8 ();
extern void isr9 ();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

/* Syscall gate – int 0x80 (vector 128). */
extern void isr128();

// Initialisation function is publicly accessible.
void init_descriptor_tables();

/*
 * tss_set_kernel_stack – update the TSS ESP0 field.
 *
 * Called by the task switcher before entering (or returning to) a task so
 * that Ring-3 → Ring-0 transitions (syscalls, IRQs) always land on the
 * correct kernel stack.
 *
 * esp0 – address of the TOP of the task's kernel stack (i.e. the value
 *         ESP should have after the CPU has pushed SS/ESP/EFLAGS/CS/EIP).
 */
void tss_set_kernel_stack(uint32_t esp0);
uint32_t tss_get_esp0(void);

#endif // DESCRIPTOR_TABLES_H