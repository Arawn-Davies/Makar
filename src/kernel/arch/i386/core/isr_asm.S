/*
 * ISR + IRQ stubs.
 *
 * Originally used GAS `.macro`, but TCC's assembler doesn't support that.
 * Rewritten as cpp `#define` so the macros expand before either assembler
 * (gas or TCC) ever sees them.  Built with -x assembler-with-cpp.
 */

#define ISR_NOERRCODE(num) \
.global isr##num         ;\
isr##num:                ;\
    cli                  ;\
    pushl $0             ;\
    pushl $num           ;\
    jmp isr_common_stub

#define ISR_ERRCODE(num) \
.global isr##num         ;\
isr##num:                ;\
    cli                  ;\
    pushl $num           ;\
    jmp isr_common_stub

#define IRQ(irq_num, isr_num) \
.global irq##irq_num     ;\
irq##irq_num:            ;\
    cli                  ;\
    pushl $0             ;\
    pushl $isr_num       ;\
    jmp irq_common_stub

ISR_NOERRCODE(0)
ISR_NOERRCODE(1)
ISR_NOERRCODE(2)
ISR_NOERRCODE(3)
ISR_NOERRCODE(4)
ISR_NOERRCODE(5)
ISR_NOERRCODE(6)
ISR_NOERRCODE(7)
ISR_ERRCODE(8)
ISR_NOERRCODE(9)
ISR_ERRCODE(10)
ISR_ERRCODE(11)
ISR_ERRCODE(12)
ISR_ERRCODE(13)
ISR_ERRCODE(14)
ISR_NOERRCODE(15)
ISR_NOERRCODE(16)
ISR_NOERRCODE(17)
ISR_NOERRCODE(18)
ISR_NOERRCODE(19)
ISR_NOERRCODE(20)
ISR_NOERRCODE(21)
ISR_NOERRCODE(22)
ISR_NOERRCODE(23)
ISR_NOERRCODE(24)
ISR_NOERRCODE(25)
ISR_NOERRCODE(26)
ISR_NOERRCODE(27)
ISR_NOERRCODE(28)
ISR_NOERRCODE(29)
ISR_NOERRCODE(30)
ISR_NOERRCODE(31)

IRQ(0,  32)
IRQ(1,  33)
IRQ(2,  34)
IRQ(3,  35)
IRQ(4,  36)
IRQ(5,  37)
IRQ(6,  38)
IRQ(7,  39)
IRQ(8,  40)
IRQ(9,  41)
IRQ(10, 42)
IRQ(11, 43)
IRQ(12, 44)
IRQ(13, 45)
IRQ(14, 46)
IRQ(15, 47)

/* Syscall gate: int 0x80 (vector 128) */
ISR_NOERRCODE(128)

#ifndef __TINYC__
.extern isr_handler              /* In isr.c -- GAS no-op; TCC asm rejects it */
#endif

/* This is our common ISR stub. It saves the processor state, sets
 * up for kernel mode segments, calls the C-level fault handler,
 * and finally restores the stack frame. */
isr_common_stub:
    pusha                        /* Pushes edi, esi, ebp, esp, ebx, edx, edx, eax */

    movw %ds, %ax                /* Lower 16-bits of eax = ds. */
    pushl %eax                   /* save the data segment descriptor */

    movw $0x10, %ax              /* load the kernel data segment descriptor */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp               /* push pointer to registers_t frame (arg to isr_handler) */
    call isr_handler
    addl $4, %esp            /* pop the pushed pointer */

    popl %eax                    /* reload the original data segment descriptor */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    popa                         /* Pops edi, esi, ebp... */
    addl $8, %esp                /* Cleans up the pushed error code and pushed ISR number */
    sti
    iret                         /* pops 5 things at once, CS, EIP, EFLAGS, SS, and ESP */

#ifndef __TINYC__
.extern irq_handler              /* In isr.c -- GAS no-op; TCC asm rejects it */
#endif

/* This is our common IRQ stub. It saves the processor state, sets
 * up for kernel mode segments, calls the C-level fault handler,
 * and finally restores the stack frame. */
irq_common_stub:
    pusha                        /* Pushes edi, esi, ebp, esp, ebx, edx, ecx, eax */

    movw %ds, %ax                /* Lower 16 bits of eax = ds. */
    pushl %eax                   /* save the data segment descriptor */

    movw $0x10, %ax              /* load the kernel data segment descriptor. */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushl %esp               /* push pointer to registers_t frame (arg to irq_handler) */
    call irq_handler
    addl $4, %esp            /* pop the pushed pointer */

    popl %ebx                    /* reload the original data segment descriptor */
    movw %bx, %ds
    movw %bx, %es
    movw %bx, %fs
    movw %bx, %gs

    popa                         /* Pops edi, esi, ebp, esp, ebx, edx, ecx, eax */
    addl $8, %esp                /* Cleans up the pushed error code and pushed IRQ number */
    sti
    iret                         /* pops 5 things at once: CS, EIP, EFLAGS, SS, and ESP */
