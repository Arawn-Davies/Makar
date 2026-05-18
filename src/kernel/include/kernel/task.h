#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>      /* VFS_PATH_MAX */
#include <kernel/fd.h>       /* fd_table_t, TASK_MAX_FDS */

/* Size of the private kernel stack allocated for each task. */
#define TASK_STACK_SIZE  8192

/* Maximum number of concurrent tasks (including the idle/kernel task). */
#define MAX_TASKS        8

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
    const char   *name;      /* points at name_buf for exec'd tasks,    *
                              * or a string literal for kernel tasks.   */
    char          name_buf[16]; /* durable storage for exec/spawn names */
    struct task  *next;      /* intrusive circular linked list                    */

    /* --- identity --- */
    int           pid;       /* unique, monotonically increasing; idle task = 1   */

    /* --- user memory --- */
    uint32_t      user_brk;  /* current user-space heap break (0 = not a user process) */

    /* --- per-task working directory ---
     * Authoritative storage for the task's cwd.  vfs_getcwd() / vfs_cd()
     * read and write this field for the calling task; relative-path resolution
     * in path_resolve() joins against it.  When the shell moves to userspace,
     * SYS_GETCWD / SYS_CHDIR will become thin wrappers over the same field -
     * no special-casing required. */
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
     * Per-task table; fds 0/1/2 pre-bound to stdin/stdout/stderr at
     * task_create. Always allocated (even for idle, since ktest runs
     * there in test_mode boots). See kernel/fd.h. */
    fd_table_t   *fd_table;

    /* --- protection ---
     * If non-zero, sig_deliver refuses to transition this task to
     * TASK_DEAD regardless of pending signal (including SIGKILL).
     * Set on the idle task (kernel would deadlock without it) and on
     * the four shell tasks (terminating one leaves its VT permanently
     * dead, since the shell pool is created once at boot).  User-
     * installed handlers still run normally; only the kernel-driven
     * termination path is gated. */
    int           unkillable;

    /* Set the first time this task calls SYS_PUTCH_AT or SYS_TTY_CLEAR.
     * Used by shell_exec_elf to decide whether to repaint after a
     * fullscreen ELF exits -- ordinary line-mode programs (cat, hello,
     * makbox fallback for typos) leave this 0, so the shell's "always
     * clean up after exec" behaviour doesn't clobber their output. */
    int           fb_touched;

    /* --- tick accounting ---
     * Cumulative PIT ticks (100 Hz) during which this task was the
     * current_task at IRQ 0 time.  Incremented by timer_callback before
     * the preemptive yield.  Rolls every ~497 days at 100 Hz; that's
     * fine for diagnostics, replace with uint64_t if real uptime SLAs
     * ever appear.  Read via /proc/tasks. */
    uint32_t      kticks;

    /* --- exec hand-off ---
     * Pointer to a heap-allocated exec_params_t set up by shell_exec_elf
     * just before task_create.  exec_task_entry reads from this field
     * (not a shared static), then kfree's it.  Per-task isolation is
     * critical: without it, two shells on different TTYs racing to exec
     * stomp each other's argv/path globals, producing corrupted argc
     * frames or jumps to garbage EIPs in ring-3.  NULL for tasks that
     * aren't exec'ing a userspace program. */
    void         *exec_params;
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
