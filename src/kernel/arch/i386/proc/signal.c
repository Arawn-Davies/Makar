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
 * The legacy g_sigint / keyboard_sigint_consume path was removed in
 * phase 3: the keyboard ISR now delivers SIGINT directly to the focused
 * task via sig_send(), shell tasks install SIG_IGN at startup so they
 * survive Ctrl+C at their own prompt, and shell_exec_elf simply yields
 * until the child reaches TASK_DEAD (the kernel does the killing).
 */

#include <kernel/signal.h>
#include <kernel/task.h>
#include <kernel/serial.h>
#include <kernel/isr.h>
#include <kernel/syscall.h>
#include <stddef.h>
#include <string.h>

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

/* ---------------------------------------------------------------------------
 * Ring-3 trampoline delivery (slice 8 phase 4)
 *
 * Called from isr.c at the end of every interrupt dispatch.  If the
 * interrupted frame is ring 3 AND the calling task has a deliverable
 * (pending & unmasked) signal with a user-installed handler, we build
 * a sigframe on the user stack and redirect regs->eip to the handler.
 * The handler returns into the trampoline at the bottom of the
 * sigframe, which executes SYS_SIGRETURN to restore state.
 *
 * Default-action / SIG_IGN cases are handled at schedule()'s
 * sig_deliver instead -- they don't need a user-stack trampoline,
 * just kernel-side state mutation.  Those run before we ever return
 * to user mode, so a default-terminate task never reaches this
 * function (it's TASK_DEAD by then).
 * --------------------------------------------------------------------------- */

/* Trampoline bytes: movl $SYS_SIGRETURN, %eax; int $0x80; nop.
 * Constant so the compiler embeds it in .rodata; we memcpy at delivery. */
static const uint8_t s_sigreturn_trampoline[8] = {
    0xb8,
    (uint8_t)(SYS_SIGRETURN      ),
    (uint8_t)(SYS_SIGRETURN >>  8),
    (uint8_t)(SYS_SIGRETURN >> 16),
    (uint8_t)(SYS_SIGRETURN >> 24),
    0xcd, 0x80,
    0x90,
};

void signal_check_user(registers_t *regs)
{
    /* Only consider tasks about to iret to ring 3.  The CS low two
     * bits encode the requested privilege level; user code segments
     * are RPL=3 (0x1B == GDT index 3 | RPL 3). */
    if ((regs->cs & 0x3u) != 0x3u)
        return;

    struct task *t = task_current();
    if (!t)
        return;

    uint32_t deliverable = t->sig_pending & ~t->sig_mask;
    if (!deliverable)
        return;

    for (int sig = 1; sig <= SIG_MAX; sig++) {
        uint32_t bit = SIG_BIT(sig);
        if (!(deliverable & bit))
            continue;
        sig_handler_t h = sig_get_handler(t, sig);
        /* SIG_DFL and SIG_IGN are sig_deliver()'s job; SIGKILL has no
         * handler override.  Anything else is a user pointer. */
        if (h == SIG_DFL || h == SIG_IGN)
            continue;
        if (sig == SIGKILL)
            continue;

        /* Place sigframe just below the current user esp.  The page is
         * mapped (it's the ring-3 stack page) so direct writes via the
         * same CR3 are safe.  Sigframe is ~70 bytes, well under one
         * page, so we don't cross the stack-page boundary in practice. */
        uint32_t base = regs->useresp - sizeof(sigframe_t);
        sigframe_t *uf = (sigframe_t *)(uintptr_t)base;

        uf->ret_addr     = base + (uint32_t)offsetof(sigframe_t, trampoline);
        uf->signo        = (uint32_t)sig;
        uf->saved_eip    = regs->eip;
        uf->saved_cs     = regs->cs;
        uf->saved_eflags = regs->eflags;
        uf->saved_useresp= regs->useresp;
        uf->saved_ss     = regs->ss;
        uf->saved_eax    = regs->eax;
        uf->saved_ebx    = regs->ebx;
        uf->saved_ecx    = regs->ecx;
        uf->saved_edx    = regs->edx;
        uf->saved_esi    = regs->esi;
        uf->saved_edi    = regs->edi;
        uf->saved_ebp    = regs->ebp;
        uf->saved_ds     = regs->ds;
        uf->magic        = SIGFRAME_MAGIC;
        memcpy(uf->trampoline, s_sigreturn_trampoline,
               sizeof(s_sigreturn_trampoline));

        /* Redirect iret to the handler with esp at the sigframe head.
         * Handler is invoked cdecl with [esp]=ret_addr, [esp+4]=signo. */
        regs->useresp = base;
        regs->eip     = (uint32_t)(uintptr_t)h;

        /* Clear pending so we don't re-deliver before the handler
         * even gets a chance to run. */
        t->sig_pending &= ~bit;

        Serial_WriteString("[signal] deliver sig=");
        Serial_WriteDec((uint32_t)sig);
        Serial_WriteString(" pid=");
        Serial_WriteDec((uint32_t)t->pid);
        Serial_WriteString(" handler=0x");
        Serial_WriteHex((uint32_t)(uintptr_t)h);
        Serial_WriteString("\n");

        return;   /* one signal per return-to-user pass */
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
