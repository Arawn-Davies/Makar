#ifndef _KERNEL_RING3_H
#define _KERNEL_RING3_H

#include <stdint.h>

/*
 * ring3_enter – drop the current task to ring 3.
 *
 * Loads the user data segment (0x23) into DS/ES/FS/GS, builds the five-word
 * iret frame on the kernel stack (SS=0x23, user ESP, EFLAGS with IF=1,
 * CS=0x1B, user EIP), and executes iret.  Never returns — the task exits via
 * SYS_EXIT (int 0x80 → task_exit()).
 *
 * Prerequisites:
 *   - The TSS esp0 must point to the top of the current task's kernel stack
 *     (call tss_set_kernel_stack() before entering).
 *   - The current CR3 must be a page directory that maps the user EIP/ESP
 *     addresses with PAGE_USER set (use vmm_switch() before entering).
 */
void __attribute__((noreturn)) ring3_enter(uint32_t user_eip, uint32_t user_esp);

#endif /* _KERNEL_RING3_H */
