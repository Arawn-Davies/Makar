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
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/paging.h>
#include <kernel/descr_tbl.h>
#include <kernel/system.h>
#include <kernel/tty.h>
#include <kernel/asm.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */

static task_t  task_pool[MAX_TASKS];
static int     task_pool_count = 0;
static task_t *current_task    = NULL;
static int     tasking_active  = 0;
static int     next_pid        = 2;   /* idle task gets PID 1; created tasks start at 2 */

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
    idle->cwd[0]   = '/';
    idle->cwd[1]   = '\0';
    idle->tty      = TASK_TTY_NONE;
    idle->sig_pending = 0;
    idle->sig_mask    = 0;
    idle->fd_table    = NULL;

    task_pool_count = 1;
    current_task    = idle;
    tasking_active  = 1;
}

task_t *task_create(const char *name, void (*entry)(void))
{
    task_t *t = NULL;

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

            /* Reuse the existing kernel stack. */
            memset(t->stack, 0, TASK_STACK_SIZE);
            break;
        }
    }

    if (!t) {
        if (task_pool_count >= MAX_TASKS)
            return NULL;

        uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
        if (!stack)
            return NULL;

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
    t->sig_pending = 0;
    t->sig_mask    = 0;
    t->fd_table    = NULL;

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

    task_t *next = current_task->next;

    /* Walk up to task_pool_count steps looking for a runnable task. */
    for (int i = 0; i < task_pool_count; i++) {
        if (next->state == TASK_READY)
            break;
        next = next->next;
    }

    if (next == current_task || next->state != TASK_READY)
        return;   /* nothing else to run */

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

    /* Reap the outgoing task's user PD if it exited. The kernel stack we are
       still running on lives in shared kernel PDEs (mirrored into every PD
       by vmm_create_pd), so freeing prev's PD after switching CR3 is safe. */
    if (prev->state == TASK_DEAD &&
        prev->page_dir &&
        prev->page_dir != paging_kernel_pd() &&
        prev->page_dir != current_task->page_dir) {
        vmm_free_pd(prev->page_dir);
        prev->page_dir = paging_kernel_pd();
    }

    task_switch(&prev->esp, current_task->esp);
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
