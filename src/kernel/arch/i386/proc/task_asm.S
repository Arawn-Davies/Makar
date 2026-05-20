/*
 * task_asm.S -- Low-level context switch for cooperative kernel multitasking.
 *
 * void task_switch(uint32_t *old_esp, uint32_t new_esp);
 *
 * Saves callee-saved registers (EDI, ESI, EBX, EBP) and EFLAGS on the
 * current stack, stores the resulting stack pointer into *old_esp, then
 * loads new_esp and restores the saved state.  The "ret" at the end returns
 * into the function that originally called task_switch() in the new task's
 * context - or into the task entry function for a brand-new task.
 *
 * Caller-saved registers (EAX, ECX, EDX) do not need to be preserved here
 * because this follows the standard cdecl calling convention.
 */

.global task_switch
task_switch:
    /* Arguments (cdecl): [esp+4] = old_esp ptr, [esp+8] = new_esp value. */
    mov  4(%esp), %eax      /* eax = &current_task->esp */
    mov  8(%esp), %ecx      /* ecx = next_task->esp     */

    /* Save callee-saved registers and flags onto the current stack. */
    pushf
    push %ebp
    push %ebx
    push %esi
    push %edi

    /* Store the current stack pointer into the old task's TCB. */
    mov  %esp, (%eax)

    /* Switch to the new task's stack. */
    mov  %ecx, %esp

    /* Restore the new task's saved state. */
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    popf

    /* Return into the new task's context (entry point for new tasks,
       or the caller of task_switch() for previously-running tasks). */
    ret

/*
 * fork_child_iret -- first-run entry for a task born of SYS_FORK.
 *
 * task_fork sets up the child's kernel stack so that when the scheduler
 * first picks the child and task_switch's `ret` fires, control lands
 * here with ESP pointing at a copy of the parent's `registers_t` frame
 * (the same one isr_common_stub built on the parent's stack during the
 * int $0x80, with EAX patched to 0 so fork() returns 0 in the child).
 *
 * This is byte-for-byte the same epilogue as isr_common_stub's tail
 * (lines 103-112 of isr_asm.S) -- restore data segments, popa, drop
 * the err_code/int_no padding, iret back to ring 3.
 *
 * Stack at entry (high → low):
 *   ss           |
 *   useresp      |  pushed by CPU at int $0x80
 *   eflags       |
 *   cs           |
 *   eip          |
 *   err_code (0) |  pushed by isr80 stub (err_code first → higher addr,
 *   int_no  (128)|                        int_no second → lower addr)
 *   eax = 0      |
 *   ecx          |
 *   edx          |
 *   ebx          |  pusha (the saved esp slot is ignored on popa)
 *   esp (ignored)|
 *   ebp          |
 *   esi          |
 *   edi          |
 *   ds           |  ← ESP
 */
.global fork_child_iret
fork_child_iret:
    popl %eax                    /* eax = ds */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    popa                         /* restores edi..eax (child sees eax=0) */
    addl $8, %esp                /* skip err_code + int_no */
    iret                         /* eip, cs, eflags, useresp, ss → ring 3 */
