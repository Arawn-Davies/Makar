/*
 * fork.c -- SYS_FORK foundation (stub + design notes).
 *
 * This is a deliberate scaffold: the syscall slot, the dispatcher hook,
 * and the design notes live in tree so the implementation can land
 * incrementally without re-deriving the architecture each time.
 *
 * Returns -ENOSYS until the four pieces below are in place.  See
 * include/kernel/fork.h for the full design rationale; this file just
 * exposes sys_fork() and a skeleton for the eager-copy variant.
 */

#include <kernel/fork.h>
#include <kernel/isr.h>
#include <kernel/task.h>
#include <kernel/vmm.h>
#include <kernel/serial.h>

/* -ENOSYS -- bash and Linux i386 use 38; we mirror to stay POSIX-shaped. */
#define ENOSYS  38u

void sys_fork(registers_t *regs)
{
    Serial_WriteString("[sys_fork] not yet implemented -- returning -ENOSYS\n");
    regs->eax = (uint32_t)(-(int32_t)ENOSYS);

    /* --- Sketch of the implementation, kept in code so it survives ---
     *
     * task_t *parent = task_current();
     * task_t *child  = task_alloc_slot();
     * if (!child) { regs->eax = (uint32_t)-1; return; }
     *
     * 1) page directory: eager copy first, COW later
     *    child->page_dir = vmm_clone_pd_user_eager(parent->page_dir);
     *
     * 2) fd_table: dup with shared file pointers (POSIX) or independent
     *    copies (simpler MVP); needs refcounting on fd_entry_t either way.
     *    child->fd_table = fd_table_dup(parent->fd_table);
     *
     * 3) signal state: dispositions inherit, pending mask clears.
     *    sig_clone(parent, child);
     *
     * 4) script_vars: deep copy each name/value pair.
     *    sh_vars_clone(parent, child);
     *
     * 5) cwd, name, tty, unkillable: shallow copy.
     *    strncpy(child->cwd, parent->cwd, sizeof(child->cwd));
     *
     * 6) trap-frame: stage a kernel stack that re-enters userland at the
     *    same regs, with EAX = 0.  Closest reference today is
     *    exec_task_entry; needs a fork twin that takes the parent's
     *    registers_t verbatim.  This is the hardest piece: get the
     *    iret frame layout wrong and the child crashes on first user
     *    instruction.
     *
     * 7) make child READY, return parent.
     *    child->state = TASK_READY;
     *    regs->eax = child->pid;
     */
}
