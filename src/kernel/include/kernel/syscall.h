#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>
#include <kernel/isr.h>

/*
 * Syscall numbers for int 0x80.
 *
 * Kernel code passes the syscall number in EAX and arguments in EBX, ECX,
 * EDX, ESI, EDI (following the Linux i386 ABI convention).  The return value
 * is placed in EAX.
 *
 * Only kernel-mode callers are supported for now; user-mode support requires
 * the IDT gate to be opened to DPL=3 and a TSS to be installed.
 */

#define SYS_EXIT    1    /* task_exit()                           */
#define SYS_WRITE   4    /* write NUL-terminated string at EBX    */
#define SYS_YIELD   158  /* task_yield()                          */
#define SYS_DEBUG   100  /* print checkpoint: EBX=uint32 value    */

/*
 * syscall_init – register int 0x80 in the interrupt-handler table.
 *
 * Must be called after init_descriptor_tables() (which installs the IDT gate)
 * and after tasking_init().
 */
void syscall_init(void);

/*
 * syscall_dispatch – the int 0x80 C handler; exposed for in-kernel testing.
 *
 * Interprets regs->eax as the syscall number, dispatches to the appropriate
 * handler, and may modify regs->eax as a return value.  Safe to call directly
 * from kernel mode (e.g. ktest) provided tasking is active.
 */
void syscall_dispatch(registers_t *regs);

#endif /* _KERNEL_SYSCALL_H */
