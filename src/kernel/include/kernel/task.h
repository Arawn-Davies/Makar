#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>      /* VFS_PATH_MAX */

/* Size of the private kernel stack allocated for each task. */
#define TASK_STACK_SIZE  8192

/* Maximum number of concurrent tasks (including the idle/kernel task). */
#define MAX_TASKS        8

/* Maximum file descriptors per task. Kept small until the FD table slice lands. */
#define TASK_MAX_FDS     16

/* TTY index meaning "not bound to any TTY". */
#define TASK_TTY_NONE    (-1)

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
} task_state_t;

typedef struct task {
    /* --- scheduler core --- */
    uint32_t      esp;       /* saved stack pointer (only valid when not running) */
    uint8_t      *stack;     /* base of allocated stack (lowest address)          */
    uint32_t     *page_dir;  /* page directory (phys == virt, identity-mapped)    */
    task_state_t  state;
    const char   *name;
    struct task  *next;      /* intrusive circular linked list                    */

    /* --- identity --- */
    int           pid;       /* unique, monotonically increasing; idle task = 1   */

    /* --- user memory --- */
    uint32_t      user_brk;  /* current user-space heap break (0 = not a user process) */

    /* --- per-task working directory ---
     * Owned by the task; resolved against by VFS calls when the per-task CWD
     * slice migrates `vfs_getcwd()` off the global `s_cwd`. Until then this
     * field is plumbed but not yet authoritative. */
    char          cwd[VFS_PATH_MAX];

    /* --- TTY binding ---
     * Index into the TTY array for input routing and (later) per-TTY screen
     * buffer ownership. TASK_TTY_NONE = no TTY (idle, ktest_bg, etc.). */
    int           tty;

    /* --- signals (Linux-style; full subsystem lands in a later slice) ---
     * sig_pending: bitmask of delivered-but-not-yet-handled signals (bit n = signal n).
     * sig_mask:    bitmask of currently blocked signals (SIGKILL/SIGSTOP cannot be blocked). */
    uint32_t      sig_pending;
    uint32_t      sig_mask;

    /* --- file descriptors ---
     * Opaque pointer until the FD table slice lands; NULL means "no fd table
     * allocated yet - fall back to legacy global keyboard/serial routing". */
    void         *fd_table;
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

/* Look up a task by PID. Returns NULL if no live task has that PID. */
task_t *task_by_pid(int pid);

/*
 * task_switch – low-level context switch (implemented in task_asm.S).
 *
 * Saves callee-saved registers and EFLAGS on the current stack and stores
 * the resulting ESP into *old_esp.  Then loads new_esp, restores the saved
 * state, and returns into the new task's context.
 */
void task_switch(uint32_t *old_esp, uint32_t new_esp);

#endif /* _KERNEL_TASK_H */
