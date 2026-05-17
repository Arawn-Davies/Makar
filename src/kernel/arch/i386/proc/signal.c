/*
 * signal.c - per-task signal subsystem (Linux i386 model, minimal core).
 *
 * Design points:
 *   - Per-task sig_pending / sig_mask bitmaps live on task_t (added in the
 *     earlier per-task plumbing slice); the per-task handler table lives
 *     here in this file's accessor, alongside the delivery + default-
 *     action machinery.
 *   - Delivery is "polled at safe points": sig_deliver() is called from
 *     schedule() on both the outgoing and incoming tasks.  Default
 *     actions that terminate transition the task to TASK_DEAD so the
 *     existing reaper picks it up via the slot-reuse path - no inline
 *     PD/fd free, which was the regression source pre-#128.
 *   - User-defined handlers are stored but not yet invoked.  Invoking
 *     them needs a ring-3 trampoline + sigreturn syscall to unwind the
 *     interrupted frame; that lands in a follow-up slice.  Until then a
 *     signal with a user handler installed stays pending - it will be
 *     delivered once the trampoline goes in, no API churn at the user
 *     interface.
 *   - SIGKILL and SIGSTOP cannot have their disposition overridden.
 *     SIGKILL always terminates regardless of mask or installed handler.
 *
 * The legacy g_sigint / keyboard_sigint_consume path is intentionally
 * untouched here; converting Ctrl+C to deliver SIGINT to the focused
 * task happens in a follow-up commit so the UI-test surface stays
 * stable while the infrastructure is verified in isolation.
 */

#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/serial.h>
#include <stddef.h>

/* Per-task handler table.  Indexed by task_pool slot, not by pid, so
 * lookups don't have to walk the live list.  Keeping it out of task_t
 * itself avoids bloating the struct used by every task accessor and
 * lets the kernel boot before signal.c has been wired up. */
extern struct task *task_get(int i);
extern int          task_count(void);

#define SIG_TABLE_MAX_TASKS 8   /* must match MAX_TASKS in task.h */

static sig_handler_t s_handlers[SIG_TABLE_MAX_TASKS][NSIG];

/* Map a task_t* back to its slot index by linear search across the
 * pool.  task_count() is tiny (≤ MAX_TASKS) so this is cheaper than
 * carrying an index field on task_t for one subsystem's use. */
static int slot_of(struct task *t)
{
    int n = task_count();
    for (int i = 0; i < n; i++) {
        if (task_get(i) == t)
            return i;
    }
    return -1;
}

/* Default action classifier.  Returns non-zero iff the default action
 * for signo is "terminate" (or "core" - we don't dump cores yet, so
 * core ≡ terminate).  Everything else is treated as "ignore" by
 * default. */
int sig_default_terminates(int signo)
{
    switch (signo) {
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGILL:
    case SIGTRAP:
    case SIGABRT:
    case SIGFPE:
    case SIGKILL:
    case SIGUSR1:
    case SIGSEGV:
    case SIGUSR2:
    case SIGPIPE:
    case SIGALRM:
    case SIGTERM:
        return 1;
    /* SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP - default is ignore/stop, not
     * terminate.  Stop semantics aren't implemented yet (no TASK_STOPPED
     * state); treat as ignore for now. */
    default:
        return 0;
    }
}

void sig_task_init(struct task *t)
{
    if (!t)
        return;
    t->sig_pending = 0;
    t->sig_mask    = 0;
    int slot = slot_of(t);
    if (slot < 0)
        return;
    for (int s = 0; s < NSIG; s++)
        s_handlers[slot][s] = SIG_DFL;
}

void sig_send(struct task *t, int signo)
{
    if (!t)
        return;
    if (signo < 1 || signo > SIG_MAX)
        return;
    if (t->state == TASK_DEAD)
        return;
    t->sig_pending |= SIG_BIT(signo);
}

int sig_send_pid(int pid, int signo)
{
    struct task *t = task_by_pid(pid);
    if (!t)
        return -1;
    sig_send(t, signo);
    return 0;
}

int sig_set_handler(struct task *t, int signo, sig_handler_t handler)
{
    if (!t)
        return -1;
    if (signo < 1 || signo > SIG_MAX)
        return -1;
    if (signo == SIGKILL || signo == SIGSTOP)
        return -1;
    int slot = slot_of(t);
    if (slot < 0)
        return -1;
    s_handlers[slot][signo] = handler;
    return 0;
}

static sig_handler_t handler_for(struct task *t, int signo)
{
    int slot = slot_of(t);
    if (slot < 0)
        return SIG_DFL;
    return s_handlers[slot][signo];
}

sig_handler_t sig_get_handler(struct task *t, int signo)
{
    if (!t || signo < 1 || signo > SIG_MAX)
        return SIG_DFL;
    return handler_for(t, signo);
}

void sig_deliver(struct task *t)
{
    if (!t || t->state == TASK_DEAD)
        return;

    uint32_t deliverable = t->sig_pending & ~t->sig_mask;
    if (!deliverable)
        return;

    for (int sig = 1; sig <= SIG_MAX; sig++) {
        uint32_t bit = SIG_BIT(sig);
        if (!(deliverable & bit))
            continue;

        /* SIGKILL ignores mask and handler entirely. */
        if (sig == SIGKILL) {
            t->sig_pending &= ~bit;
            t->state = TASK_DEAD;
            Serial_WriteString("[signal] pid=");
            Serial_WriteDec((uint32_t)t->pid);
            Serial_WriteString(" terminated by SIGKILL\n");
            return;
        }

        sig_handler_t h = handler_for(t, sig);

        if (h == SIG_IGN) {
            t->sig_pending &= ~bit;
            continue;
        }

        if (h == SIG_DFL) {
            t->sig_pending &= ~bit;
            if (sig_default_terminates(sig)) {
                t->state = TASK_DEAD;
                Serial_WriteString("[signal] pid=");
                Serial_WriteDec((uint32_t)t->pid);
                Serial_WriteString(" terminated by signal ");
                Serial_WriteDec((uint32_t)sig);
                Serial_WriteString("\n");
                return;
            }
            /* Default action is ignore for the rest. */
            continue;
        }

        /* User handler installed.  Trampoline + sigreturn not yet
         * implemented; leave the bit pending so a future delivery
         * round picks it up once that lands.  Stop iterating so we
         * don't spin processing this same signal repeatedly in a
         * single delivery pass. */
        return;
    }
}

int sig_check_and_clear(int signo)
{
    if (signo < 1 || signo > SIG_MAX)
        return 0;
    struct task *t = task_current();
    if (!t)
        return 0;
    uint32_t bit = SIG_BIT(signo);
    if (!(t->sig_pending & bit))
        return 0;
    t->sig_pending &= ~bit;
    return 1;
}
