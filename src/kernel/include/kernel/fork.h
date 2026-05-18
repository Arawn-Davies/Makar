/*
 * fork.h -- foundation for SYS_FORK.
 *
 * SYS_FORK is RESERVED but UNIMPLEMENTED.  Calling it returns -ENOSYS (-38).
 *
 * Why a foundation PR rather than a working fork()?
 *
 * Implementing fork() on Makar requires four orthogonal pieces:
 *
 *   1.  COW page-directory clone.  The kernel needs to walk the parent's
 *       PD, mark every user PTE read-only in BOTH parent and child, and
 *       install a page-fault handler that splits the page on first write.
 *       (Eager-copy is an acceptable first step but doubles working-set
 *       per fork.)  vmm.c currently has vmm_clone_pd() for the kernel
 *       half only; needs a user-half twin.
 *
 *   2.  fd-table dup.  Each task owns a fd_table_t (kernel/fd.h).  POSIX
 *       semantics say the child sees the SAME open files as the parent,
 *       with shared file pointers.  The current eager fd_table_destroy
 *       model needs reference counting before fork can share entries.
 *
 *   3.  Trap-frame surgery.  At SYS_FORK entry the kernel is inside
 *       syscall_dispatch with a registers_t* describing the parent's
 *       ring-3 state.  The child task needs a kernel stack laid out so
 *       that the same isr_common_stub iret epilogue resumes it in ring 3
 *       with EAX = 0 and the parent's EIP/ESP otherwise preserved.
 *       Closest analogue today is exec_task_entry; needs a fork twin.
 *
 *   4.  Signal state + script_vars dup.  Per-task tables (signal handler
 *       array, script_vars) should clone (signals: same dispositions;
 *       script_vars: deep-copy the table).  Mechanical once 1-3 are done.
 *
 * Plus the integration testing surface: a sigtest-style userland ELF that
 * fork()s, asserts pid != 0 in parent, pid == 0 in child, both exit, and
 * the parent waits on the child.  Needs SYS_WAITPID, which is its own
 * follow-up.
 *
 * Until then this header defines the API and the syscall returns -ENOSYS
 * so userspace can probe for the feature without crashing.
 */
#ifndef MAKAR_FORK_H
#define MAKAR_FORK_H

#include <stdint.h>

struct registers;   /* arch/i386/include/kernel/isr.h */

/* sys_fork -- invoked from syscall_dispatch on SYS_FORK.
 *
 * Returns the new child's pid in regs->eax on success.
 * Returns -ENOSYS (== (uint32_t)-38) until the implementation lands.
 *
 * On success, the kernel arranges for the child task to resume in ring
 * 3 at the same EIP/ESP as the parent, with EAX = 0.  The parent's
 * regs->eax is set to the child's pid before this function returns. */
void sys_fork(struct registers *regs);

#endif /* MAKAR_FORK_H */
