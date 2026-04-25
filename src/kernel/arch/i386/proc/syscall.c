/*
 * syscall.c -- int 0x80 syscall dispatcher.
 *
 * The IDT gate for vector 128 (0x80) is installed by init_descriptor_tables()
 * in descr_tbl.c.  This file registers the C-level handler via the existing
 * interrupt-handler table so that isr_handler() dispatches to syscall_dispatch.
 *
 * Calling convention (Linux i386 ABI):
 *   EAX = syscall number
 *   EBX = arg1, ECX = arg2, EDX = arg3, ESI = arg4, EDI = arg5
 *
 * Return value: written to regs->eax so the caller sees it in EAX after iret.
 */

#include <kernel/syscall.h>
#include <kernel/isr.h>
#include <kernel/task.h>
#include <kernel/tty.h>
#include <kernel/serial.h>

void syscall_dispatch(registers_t *regs)
{
    switch (regs->eax) {
    case SYS_EXIT:
        task_exit();
        /* task_exit() does not return */
        break;

    case SYS_WRITE:
        /* EBX = pointer to NUL-terminated string */
        if (regs->ebx)
            t_writestring((const char *)(uintptr_t)regs->ebx);
        break;

    case SYS_YIELD:
        task_yield();
        break;

    case SYS_DEBUG:
        t_writestring("[ring3] CP: 0x");
        t_hex(regs->ebx);
        t_putchar('\n');
        Serial_WriteString("[ring3] CP: 0x");
        Serial_WriteHex(regs->ebx);
        Serial_WriteString("\n");
        break;

    default:
        /* Unknown syscall – silently ignore. */
        break;
    }
}

void syscall_init(void)
{
    register_interrupt_handler(0x80, syscall_dispatch);
}
