#ifndef _KERNEL_SIGNAL_H
#define _KERNEL_SIGNAL_H

#include <stdint.h>

/* Linux i386 signal numbers.  We only enumerate the ones the kernel
 * currently inspects; the rest of the standard set is reserved by
 * number so future code can use SIGFOO without churning headers. */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

/* Number of signal slots carried in the per-task pending/mask bitmaps
 * and in the per-task handler table.  Bit (n-1) corresponds to signal
 * n, so this is one past the highest signal number we support. */
#define SIG_MAX  31
#define NSIG     (SIG_MAX + 1)

/* Bitmask helper: SIG_BIT(SIGINT) == (1 << 1). */
#define SIG_BIT(n)  (1u << ((n) - 1))

/* Special handler sentinels matching the POSIX SIG_DFL / SIG_IGN ABI:
 * the kernel checks pointer identity, never dereferences these values. */
typedef void (*sig_handler_t)(int signo);

#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* Forward declaration to avoid pulling task.h into every signal user. */
struct task;

/* sig_task_init - reset a task's signal state to defaults.
 *
 * Called from task_create and tasking_init.  Clears pending/mask and
 * sets every handler slot to SIG_DFL. */
void sig_task_init(struct task *t);

/* sig_send - post a signal to a target task.
 *
 * Sets the pending bit; actual effect (default action / mask check /
 * future handler invocation) happens at the next delivery point on the
 * target task (currently: top of schedule() for both the outgoing and
 * incoming tasks).  Safe to call from IRQ context.
 *
 * No-op if t is NULL, if signo is out of range, or if t is DEAD. */
void sig_send(struct task *t, int signo);

/* sig_send_pid - convenience wrapper around sig_send that resolves a
 * pid via task_by_pid().  Returns 0 on success, -1 if no task with
 * that pid exists. */
int sig_send_pid(int pid, int signo);

/* sig_deliver - process any unmasked pending signals on t.
 *
 * If a default-terminate action fires, the task's state transitions
 * to TASK_DEAD; the scheduler / slot-reuse path then reaps it.
 * User-defined handlers are not yet invoked - a signal with a user
 * handler installed is left pending (to be picked up once trampoline
 * + sigreturn lands).  No-op if t is NULL or DEAD. */
void sig_deliver(struct task *t);

/* sig_set_handler - install a handler for signo on task t.
 *
 * SIG_DFL / SIG_IGN are accepted.  SIGKILL and SIGSTOP cannot have
 * their handler overridden (returns -1 in that case).  Returns 0 on
 * success, -1 on error. */
int sig_set_handler(struct task *t, int signo, sig_handler_t handler);

/* sig_get_handler - read the currently installed handler for signo
 * on task t.  Returns SIG_DFL on any error (out-of-range signo,
 * NULL task, or task not in the pool) -- callers that care can
 * range-check signo themselves first. */
sig_handler_t sig_get_handler(struct task *t, int signo);

/* sig_check_and_clear - test and clear a pending signal on the calling
 * task.  Returns 1 if the bit was set (and clears it), 0 otherwise.
 * Useful for tasks that want to react to a signal without exposing
 * themselves to the default-terminate action -- install SIG_IGN first,
 * then poll this. */
int sig_check_and_clear(int signo);

/* sig_default_terminates - true if the default action for signo is to
 * terminate the receiving task.  Used by sig_deliver and exposed for
 * ktest. */
int sig_default_terminates(int signo);

#endif /* _KERNEL_SIGNAL_H */
