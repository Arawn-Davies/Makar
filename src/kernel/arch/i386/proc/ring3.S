.section .text
.global ring3_enter

/*
 * ring3_enter(uint32_t user_eip, uint32_t user_esp)
 *
 * Builds a five-word iret frame on the kernel stack and executes iret to
 * transfer control to ring-3 code.
 *
 * Stack layout on entry (cdecl):
 *   [esp+0]  return address (unused - this function never returns)
 *   [esp+4]  user_eip
 *   [esp+8]  user_esp
 *
 * GDT selectors:
 *   0x1B = index 3, RPL=3 - user code  segment
 *   0x23 = index 4, RPL=3 - user data segment
 */
ring3_enter:
    /* Load arguments into registers before touching the stack. */
    mov 4(%esp), %ecx       /* user_eip */
    mov 8(%esp), %edx       /* user_esp */

    /* Switch segment registers to the user data selector.
     * The CPU checks DPL >= CPL on explicit segment loads, but we are still
     * at CPL=0 here so loading a DPL=3 descriptor is permitted. */
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Push the iret frame (iret pops EIP, CS, EFLAGS, then ESP, SS). */
    push $0x23              /* SS  - user stack segment        */
    push %edx               /* ESP - user stack pointer        */
    pushf                   /* EFLAGS                          */
    orl  $0x200, (%esp)     /* force IF=1 (interrupts enabled) */
    push $0x1B              /* CS  - user code segment, RPL=3  */
    push %ecx               /* EIP - user entry point          */

    iret
