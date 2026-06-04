#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/vfs.h>      /* VFS_PATH_MAX */
#include <kernel/fd.h>       /* fd_table_t, TASK_MAX_FDS */
#include <kernel/isr.h>      /* registers_t for task_fork */
#include <kernel/ipc.h>      /* ipc_msg_t + IPC_STATE_* for the IPC fields */

/* Size of the private kernel stack allocated for each task. */
#define TASK_STACK_SIZE  8192

/* Maximum number of concurrent tasks (including the idle/kernel task).
 * Baseline runtime load is idle + 4 VT shells = 5; a ring-3 app on every
 * VT adds 4 more, and boot briefly runs the ktest task too.  8 was too
 * tight (a 4th concurrent app couldn't be created), so allow generous
 * headroom. */
#define MAX_TASKS        32

/* TTY index meaning "not bound to any TTY". */
#define TASK_TTY_NONE    (-1)

typedef enum {
    TASK_READY   = 0,
    TASK_RUNNING = 1,
    TASK_DEAD    = 2,
    /* TASK_ZOMBIE -- task has called task_exit but its parent hasn't yet
     * wait4'd it.  Slot is held with exit_status preserved; PD is
     * reaped on the next schedule() (same logic as DEAD).  Distinguished
     * from DEAD so task_create's reclaim path won't reuse the slot
     * before the parent reads the status. */
    TASK_ZOMBIE  = 3,
    /* TASK_BLOCKED -- parked in a synchronous IPC rendezvous (send waiting
     * for a receiver, or recv waiting for a sender).  The scheduler only
     * runs TASK_READY tasks, so a blocked task is simply skipped; its
     * rendezvous partner flips it back to TASK_READY on wake.  See ipc.c. */
    TASK_BLOCKED = 4,
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
    int           parent_pid;/* pid of creating task (0 = no parent / kernel)     */

    /* --- exit ---
     * Filled in by SYS_EXIT before transitioning to TASK_ZOMBIE; read by
     * a wait4-ing parent via SYS_WAIT4. */
    int           exit_status;

    /* --- user memory --- */
    uint32_t      user_brk;  /* current user-space heap break (0 = not a user process) */
    uint32_t      mmap_next; /* bump pointer for anonymous mmap (0 = lazily init to USER_MMAP_BASE) */
    uint8_t       fpu_state[512] __attribute__((aligned(16))); /* x87/SSE fxsave area, saved/restored on context switch */
    /* Thread-Local Storage (set_thread_area): per-task %gs.  tls_gs defaults
     * to the user-data selector 0x23; set_thread_area switches it to 0x33 and
     * fills base/limit so the scheduler reloads the GDT TLS slot on switch. */
    uint32_t      tls_gs;     /* %gs selector for this task (default 0x23)         */
    uint32_t      tls_base;   /* TLS segment base (thread-pointer block)           */
    uint32_t      tls_limit;  /* TLS segment limit                                 */
    uint8_t       tls_pages;  /* limit granularity: 1 = pages, 0 = bytes           */
    uint8_t       tls_active; /* 1 once set_thread_area ran                        */

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

    /* Display colors saved at the point this task forked a child.
     * SYS_WAIT4 reads these back when a fb_touched child is reaped so the
     * parent's colour scheme is restored before the screen is cleared --
     * otherwise the VT children's palette (e.g. makmux's green-on-black)
     * would bleed into the parent's next prompt. */
    uint32_t      disp_fg_saved;
    uint32_t      disp_bg_saved;

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

    /* --- shell scripting state ---
     * Per-shell-task variable table for sh-style scripting (NAME=value,
     * $VAR expansion, `read` builtin).  Each VT's shell owns its own
     * table; variables don't leak across VTs.  NULL for non-shell tasks.
     * Allocated lazily on first assignment by sh_vars_set().  See
     * src/kernel/arch/i386/shell/sh_vars.c. */
    void         *script_vars;

    /* --- IPC (microkernel synchronous message passing) ---
     * ipc_buf:       kernel-resident staging buffer for the in-flight message
     *                (decouples sender/receiver address spaces).
     * ipc_state:     IPC_STATE_IDLE / SENDING / RECVING.
     * ipc_partner:   dest pid while SENDING, or src filter (pid / IPC_ANY)
     *                while RECVING.
     * ipc_rc:        result handed back to this task when it is woken from a
     *                blocked send/recv (0, or -ESRCH if the partner died).
     * ipc_sender_q:  head of the list of tasks blocked SENDING to this task.
     * ipc_sq_next:   this task's link while queued in some receiver's
     *                ipc_sender_q.
     * See arch/i386/proc/ipc.c. */
    ipc_msg_t     ipc_buf;
    int           ipc_state;
    int           ipc_partner;
    int           ipc_rc;
    struct task  *ipc_sender_q;
    struct task  *ipc_sq_next;
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

/*
 * task_terminate – record exit_status on task `t` and transition it to
 * TASK_ZOMBIE (if it has a live ring-3 parent that may wait4 it) or
 * TASK_DEAD otherwise.  Shared by task_exit (self-exit) and sig_deliver
 * (one task killing another via a default-terminate signal) so a
 * signal-killed ring-3 child is reapable by its wait4-ing parent exactly
 * like a self-exited one -- which is what lets the parent shell reclaim
 * keyboard focus after Ctrl+C kills its foreground child.
 */
void task_terminate(task_t *t, int status);

/* Returns a pointer to the currently running task, or NULL before tasking_init. */
task_t *task_current(void);

/* Returns the entry at index i in the task pool (for shell diagnostics). */
task_t *task_get(int i);

/* Returns the total number of task slots allocated so far. */
int task_count(void);

/* Look up a task by PID. Returns NULL if no live task has that PID. */
task_t *task_by_pid(int pid);

/*
 * task_is_admin – the single privilege check for admin syscalls.
 *
 * Makar has no user model yet, so this currently returns 1 for every
 * task (boot tasks, shell-spawned children, ktest, idle).  When a real
 * login/uid model lands, this is the one place that gains the actual
 * uid/cap check — every admin syscall calls through here so the
 * authorization surface stays auditable.
 *
 * Returns 1 if `t` (NULL = task_current()) may invoke privileged
 * syscalls, 0 otherwise.
 */
int task_is_admin(task_t *t);

/*
 * task_fork – COW-clone the calling task.
 *
 * Implements the kernel side of SYS_FORK.  The parent's user address
 * space is duplicated via vmm_clone_pd_cow; the fd table is deep-copied
 * via fd_table_clone; cwd / tty / signal handlers / user_brk are inherited.
 *
 * `parent_regs` is the registers_t frame the int 0x80 dispatcher received
 * from the parent; the child's kernel stack is initialised so that its
 * first task_switch lands in fork_child_iret with a copy of this frame
 * (EAX patched to 0) ready to iret back to ring 3.
 *
 * Returns the child task (parent should put child->pid into regs->eax),
 * or NULL on PMM/heap exhaustion or full task pool.
 */
task_t *task_fork(registers_t *parent_regs);

/*
 * task_switch – low-level context switch (implemented in task_asm.S).
 *
 * Saves callee-saved registers and EFLAGS on the current stack and stores
 * the resulting ESP into *old_esp.  Then loads new_esp, restores the saved
 * state, and returns into the new task's context.
 */
void task_switch(uint32_t *old_esp, uint32_t new_esp);

#endif /* _KERNEL_TASK_H */
