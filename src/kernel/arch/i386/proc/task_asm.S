/*
 * task_asm.S -- Low-level context switch for cooperative kernel multitasking.
 *
 * void task_switch(uint32_t *old_esp, uint32_t new_esp);
 *
 * Saves callee-saved registers (EDI, ESI, EBX, EBP) and EFLAGS on the
 * current stack, stores the resulting stack pointer into *old_esp, then
 * loads new_esp and restores the saved state.  The "ret" at the end returns
 * into the function that originally called task_switch() in the new task's
 * context — or into the task entry function for a brand-new task.
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
