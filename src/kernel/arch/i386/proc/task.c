/*
 * task.c -- Cooperative kernel multitasking.
 *
 * Implements a simple round-robin scheduler over a fixed pool of kernel tasks.
 * Tasks voluntarily yield the CPU via task_yield(); no preemptive switching
 * occurs inside interrupt handlers, so the IRQ frame and task frame never mix.
 *
 * The spinner in tty.c continues to be driven directly by the PIT IRQ callback
 * (timer.c) and therefore updates on every tick regardless of which task is
 * currently executing.
 */

#include <kernel/task.h>
#include <kernel/fd.h>
#include <kernel/signal.h>
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/paging.h>
#include <kernel/descr_tbl.h>
#include <kernel/system.h>
#include <kernel/tty.h>
#include <kernel/asm.h>
#include <kernel/serial.h>
#include <kernel/vfs.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */

static task_t  task_pool[MAX_TASKS];
static int     task_pool_count = 0;
static task_t *current_task    = NULL;
static int     tasking_active  = 0;
static int     next_pid        = 2;   /* idle task gets PID 1; created tasks start at 2 */

/* schedule() re-entrancy guard.  Single-CPU only; an MP port would need
 * this per-CPU and protected by a different primitive.  Set to 1 across
 * schedule()'s critical section; checked at entry to bail on nested
 * calls (either an IRQ that managed to land during the pre-cli window
 * or some future kernel code that calls task_yield() from inside one of
 * the helpers schedule() invokes).
 *
 * Without this, the scheduler relied entirely on cli/sti discipline:
 * the timer IRQ runs with IF=0 from isr_common_stub, so re-entry via
 * IRQ was already prevented in practice -- but the guarantee was
 * implicit and one stray sti would have re-opened the hole.  The flag
 * makes the invariant explicit so future code can't accidentally undo
 * it. */
static volatile int in_schedule = 0;

/* Save EFLAGS and disable interrupts; return the prior EFLAGS so the
 * caller can restore exactly the IF state it had on entry. */
static inline uint32_t irq_save_disable(void)
{
    uint32_t f;
    asm volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint32_t f)
{
    asm volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

/* -------------------------------------------------------------------------
 * init_task_stack
 *
 * Pre-populates the stack of a brand-new task so that the first call to
 * task_switch() into this task behaves as if task_switch() was called from
 * within the entry function.
 *
 * task_switch() saves: pushf, push ebp, push ebx, push esi, push edi.
 * It then does: pop edi, pop esi, pop ebx, pop ebp, popf, ret.
 *
 * The "ret" pops the top-of-stack as the return address, so we place the
 * entry function pointer there.  Below that we place task_exit() so that if
 * entry() ever returns the task is cleanly terminated.
 *
 * Stack layout (high → low, ESP points to the lowest element):
 *
 *   task_exit   <- if entry() returns (below entry on the stack)
 *   entry       <- popped by "ret" in task_switch
 *   eflags=0x202<- popf  (IF=1, bit 1 always 1)
 *   ebp=0
 *   ebx=0
 *   esi=0
 *   edi=0       <- initial ESP
 * ------------------------------------------------------------------------- */
static void init_task_stack(task_t *t, void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)(t->stack + TASK_STACK_SIZE);

    *--sp = (uint32_t)(uintptr_t)task_exit;   /* fallback if entry() returns */
    *--sp = (uint32_t)(uintptr_t)entry;       /* return address for ret      */
    *--sp = 0x00000202u;                      /* EFLAGS: IF=1, reserved=1    */
    *--sp = 0;                                /* ebp                         */
    *--sp = 0;                                /* ebx                         */
    *--sp = 0;                                /* esi                         */
    *--sp = 0;                                /* edi                         */

    t->esp = (uint32_t)(uintptr_t)sp;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void tasking_init(void)
{
    task_t *idle = &task_pool[0];

    /* task_pool[0] represents the current execution context (idle/kernel). */
    memset(idle, 0, sizeof(*idle));
    idle->esp      = 0;                  /* filled in by task_switch on first yield */
    idle->stack    = NULL;               /* uses the original boot stack             */
    idle->page_dir = paging_kernel_pd(); /* shares the kernel address space          */
    idle->state    = TASK_RUNNING;
    idle->name     = "idle";
    idle->next     = idle;               /* circular list of one for now             */
    idle->pid      = 1;
    idle->user_brk = 0;
    /* Seed idle->cwd from the boot-time scratch cwd that vfs_init() /
     * vfs_auto_mount() populated (e.g. "/cdrom" on a CD-ROM boot).  This
     * is the one-shot handoff: from this point onward vfs_getcwd() routes
     * to task_current()->cwd, and the boot scratch buffer is unused. */
    {
        const char *boot_cwd = vfs_getcwd();
        size_t n = strlen(boot_cwd);
        if (n >= VFS_PATH_MAX) n = VFS_PATH_MAX - 1;
        memcpy(idle->cwd, boot_cwd, n);
        idle->cwd[n] = '\0';
    }
    idle->tty      = TASK_TTY_NONE;
    /* The idle task is unkillable: a SIGKILL/SIGTERM landing on it
     * would mark it DEAD and the kernel has no fallback runnable when
     * idle is gone (the scheduler bails out and the system hangs). */
    idle->unkillable = 1;
    /* sig_task_init zeroes sig_pending/sig_mask and resets the per-task
     * handler table held in signal.c to SIG_DFL across all signals. */
    sig_task_init(idle);
    /* Idle gets the default stdin/stdout/stderr table too: test_mode
     * boots run ktest_run_all from this context, and any code path that
     * dispatches int 0x80 from the idle context (ktests, in-kernel
     * diagnostics) expects fds 0/1/2 to resolve. The cost is one
     * fd_table_t (~520 B) for the lifetime of the kernel. */
    idle->fd_table    = fd_table_create_default();
    idle->exec_params = NULL;

    task_pool_count = 1;
    current_task    = idle;
    tasking_active  = 1;
}

task_t *task_create(const char *name, void (*entry)(void))
{
    task_t *t = NULL;

    /* Allocate the fd table up front so an OOM here can't leave a slot
     * unlinked from the circular task list (the reclaim path mutates the
     * list before populating, and a half-initialised DEAD slot off-list
     * would loop-forever the next reclaim's "while prev->next != t"). */
    fd_table_t *new_fds = fd_table_create_default();
    if (!new_fds)
        return NULL;

    /* Try to reclaim a DEAD slot before allocating a new one. */
    for (int i = 1; i < task_pool_count; i++) {
        if (task_pool[i].state == TASK_DEAD) {
            t = &task_pool[i];

            /* Unlink from the circular list so we can re-insert after current. */
            task_t *prev = current_task;
            while (prev->next != t)
                prev = prev->next;
            prev->next = t->next;

            /* Defensively free a user PD that the scheduler reaper missed. */
            if (t->page_dir && t->page_dir != paging_kernel_pd())
                vmm_free_pd(t->page_dir);

            /* Reap the dead task's fd table (mirrors the PD reaper - we
             * deliberately don't free it from task_exit to avoid running
             * kfree from a context that's about to switch stacks). */
            if (t->fd_table) {
                fd_table_destroy(t->fd_table);
                t->fd_table = NULL;
            }

            /* Reap an exec_params buffer that the child task never
             * consumed (e.g. it was killed before exec_task_entry ran). */
            if (t->exec_params) {
                kfree(t->exec_params);
                t->exec_params = NULL;
            }

            /* Reuse the existing kernel stack. */
            memset(t->stack, 0, TASK_STACK_SIZE);
            break;
        }
    }

    if (!t) {
        if (task_pool_count >= MAX_TASKS) {
            fd_table_destroy(new_fds);
            return NULL;
        }

        uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
        if (!stack) {
            fd_table_destroy(new_fds);
            return NULL;
        }

        t = &task_pool[task_pool_count++];
        /* Zero a freshly-claimed slot before populating, so unset fields
         * (cwd[], sig_*, fd_table) start in a known state. */
        memset(t, 0, sizeof(*t));
        t->stack = stack;
    }

    t->page_dir    = paging_kernel_pd();
    t->state       = TASK_READY;
    t->name        = name;
    t->user_brk    = 0;
    t->pid         = next_pid++;
    t->kticks      = 0;
    t->unkillable  = 0;     /* default: ordinary task, no protection   */
    t->fd_table    = new_fds;
    t->exec_params = NULL;

    /* Reset signal state.  Must run after the slot is on the task pool
     * (either freshly allocated above or reclaimed from a DEAD slot) so
     * sig_task_init's slot_of() lookup in signal.c finds it. */
    sig_task_init(t);

    /* Inherit CWD and TTY binding from the creating task, mirroring POSIX
     * fork-then-exec semantics. */
    if (current_task) {
        size_t n = strlen(current_task->cwd);
        if (n >= VFS_PATH_MAX) n = VFS_PATH_MAX - 1;
        memcpy(t->cwd, current_task->cwd, n);
        t->cwd[n] = '\0';
        t->tty    = current_task->tty;
    } else {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
        t->tty    = TASK_TTY_NONE;
    }

    init_task_stack(t, entry);

    t->next            = current_task->next;
    current_task->next = t;

    return t;
}

task_t *task_by_pid(int pid)
{
    for (int i = 0; i < task_pool_count; i++) {
        task_t *t = &task_pool[i];
        if (t->pid == pid && t->state != TASK_DEAD)
            return t;
    }
    return NULL;
}

task_t *task_current(void)
{
    return current_task;
}

task_t *task_get(int i)
{
    if (i < 0 || i >= task_pool_count)
        return NULL;
    return &task_pool[i];
}

int task_count(void)
{
    return task_pool_count;
}

/* -------------------------------------------------------------------------
 * schedule – internal round-robin scheduler.
 *
 * Walks the circular task list looking for the next READY task.  If no other
 * runnable task exists, returns without switching (the current task keeps
 * running).
 * ------------------------------------------------------------------------- */
static void schedule(void)
{
    if (!tasking_active || task_pool_count < 2)
        return;

    uint32_t saved_eflags = irq_save_disable();

    /* Re-entrancy guard.  If we got here while another schedule() is
     * already in flight on this CPU (timer IRQ that beat the cli, or a
     * helper that recursed), bail without doing anything -- the outer
     * schedule() will finish its work.  The caller's IF state is
     * restored so we don't silently leave IRQs masked on the way out. */
    if (in_schedule) {
        irq_restore(saved_eflags);
        return;
    }
    in_schedule = 1;

    /* Signal delivery point #1: outgoing task.  A default-terminate
     * disposition transitions current_task to TASK_DEAD here, after
     * which the runnable-task search below naturally walks past it. */
    if (current_task->state == TASK_RUNNING)
        sig_deliver(current_task);

    task_t *next = current_task->next;

    /* Walk up to task_pool_count steps looking for a runnable task.
     * sig_deliver on each candidate so a pending terminate-signal that
     * arrived while the task was READY (e.g. via sig_send from another
     * task or an IRQ) is honoured before we resume it. */
    for (int i = 0; i < task_pool_count; i++) {
        if (next->state == TASK_READY) {
            sig_deliver(next);
            if (next->state == TASK_READY)
                break;
        }
        next = next->next;
    }

    if (next == current_task || next->state != TASK_READY) {
        in_schedule = 0;
        irq_restore(saved_eflags);
        return;   /* nothing else to run */
    }

    task_t *prev = current_task;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;

    current_task        = next;
    current_task->state = TASK_RUNNING;

    /* Update the TSS so Ring-3 → Ring-0 transitions land on this task's
       kernel stack.  Skip the idle task which uses the original boot stack. */
    if (current_task->stack)
        tss_set_kernel_stack((uint32_t)(current_task->stack + TASK_STACK_SIZE));

    /* Switch address space only when it actually changes, to avoid an
       unnecessary TLB flush between kernel tasks sharing the kernel PD. */
    if (current_task->page_dir != prev->page_dir)
        vmm_switch(current_task->page_dir);

    /* Detach the outgoing task's user PD if it exited.  We deliberately do
     * NOT free the PD here; freeing it inline in schedule() was the source
     * of two distinct regressions:
     *   1. Pre-#128: a UAF caused by a timer IRQ landing inside vmm_free_pd
     *      and re-entering schedule() with state==DEAD.
     *   2. Post-#128 (with cli around the free): test runs would either
     *      enter a TF=1 single-step storm after the reap or hang silently
     *      with no panic and no further serial output.
     * Both regressions occur after a ring-3 child task exits.  The PD is
     * instead freed by task_create's slot-reuse path (below), which runs
     * in stable kernel context with no impending CR3/stack swap.  Up to
     * MAX_TASKS - live_count dead PDs may be held briefly; with MAX_TASKS=8
     * and ~5 boot tasks that is at most ~12 KiB and is bounded. */
    if (prev->state == TASK_DEAD &&
        prev->page_dir &&
        prev->page_dir != paging_kernel_pd() &&
        prev->page_dir != current_task->page_dir) {
        Serial_WriteString("[reaper] deferring PD free for dead pid=");
        Serial_WriteDec((uint32_t)prev->pid);
        Serial_WriteString(" (reclaimed on slot reuse)\n");
        /* Leave prev->page_dir pointing at the dead PD so task_create's
         * defensive free-on-reuse picks it up.  CR3 is already on
         * current_task->page_dir from the vmm_switch above, so the
         * dead PD is not the active address space. */
    }

    /* Release the re-entrancy guard BEFORE swapping stacks.  Fresh
     * tasks start from the frame init_task_stack baked into their
     * kernel stack and never run this schedule() epilogue -- their
     * first execution jumps straight to the entry function via
     * task_switch's ret.  If we cleared the flag only after
     * task_switch, the first time we scheduled into a new task the
     * flag would stay 1 forever and every subsequent schedule() would
     * bail at the re-entry check, deadlocking the kernel after the
     * first context switch.  Re-entered tasks (the not-fresh path)
     * resume after task_switch and just restore IF -- the flag is
     * already clear so there's nothing more to do. */
    in_schedule = 0;
    task_switch(&prev->esp, current_task->esp);

    /* Re-entered task: restore the IF state we had on entry to this
     * invocation of schedule().  task_switch's popfl already restored
     * THIS task's EFLAGS to what was saved when we called task_switch
     * (IF=0 from our irq_save_disable), so we still need this. */
    irq_restore(saved_eflags);
}

void task_yield(void)
{
    if (!tasking_active)
        return;
    schedule();
}

void __attribute__((noreturn)) task_exit(void)
{
    disable_interrupts();
    if (current_task)
        current_task->state = TASK_DEAD;
    enable_interrupts();

    /* Keep rescheduling until another runnable task takes over. */
    for (;;)
        schedule();
}
