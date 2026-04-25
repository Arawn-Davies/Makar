#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>

/* Size of the private kernel stack allocated for each task. */
#define TASK_STACK_SIZE  8192

/* Maximum number of concurrent tasks (including the idle/kernel task). */
#define MAX_TASKS        8

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
} task_state_t;

typedef struct task {
    uint32_t      esp;       /* saved stack pointer (only valid when not running) */
    uint8_t      *stack;     /* base of allocated stack (lowest address)          */
    uint32_t     *page_dir;  /* page directory (phys == virt, identity-mapped)    */
    task_state_t  state;
    const char   *name;
    struct task  *next;      /* intrusive circular linked list                    */
} task_t;

/*
 * tasking_init – initialise the multitasking subsystem.
 *
 * Registers the current execution context as task 0 ("idle").
 * Must be called after heap_init() and before task_create().
 */
void tasking_init(void);

/*
 * task_create – create a new kernel task.
 *
 * Allocates a private stack and sets up the initial register frame so that
 * the first context-switch into this task begins execution at entry().
 * Returns NULL if the task pool is full or memory allocation failed.
 */
task_t *task_create(const char *name, void (*entry)(void));

/*
 * task_yield – voluntarily give up the CPU.
 *
 * Picks the next READY task in round-robin order and switches to it.
 * Safe to call before tasking_init() (becomes a no-op).
 */
void task_yield(void);

/*
 * task_exit – terminate the calling task.
 *
 * Marks the task as DEAD and transfers control to the next runnable task.
 * Does not return.
 */
void __attribute__((noreturn)) task_exit(void);

/* Returns a pointer to the currently running task, or NULL before tasking_init. */
task_t *task_current(void);

/* Returns the entry at index i in the task pool (for shell diagnostics). */
task_t *task_get(int i);

/* Returns the total number of task slots allocated so far. */
int task_count(void);

/*
 * task_switch – low-level context switch (implemented in task_asm.S).
 *
 * Saves callee-saved registers and EFLAGS on the current stack and stores
 * the resulting ESP into *old_esp.  Then loads new_esp, restores the saved
 * state, and returns into the new task's context.
 */
void task_switch(uint32_t *old_esp, uint32_t new_esp);

#endif /* _KERNEL_TASK_H */
