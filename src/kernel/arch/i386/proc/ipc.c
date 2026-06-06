#include <kernel/ipc.h>
#include <kernel/task.h>
#include <kernel/serial.h>
#include <string.h>

/* errno-style return codes (matches the small negatives used elsewhere). */
#define IPC_ESRCH   (-3)    /* no such / dead destination task */
#define IPC_EAGAIN  (-11)   /* nbrecv: nothing queued right now */
#define IPC_EFAULT  (-14)   /* bad message pointer             */
#define IPC_EINVAL  (-22)   /* invalid argument (e.g. self-send) */

/* Local IRQ save/restore -- task.c's helpers are file-static, so the IPC
 * critical sections use their own.  Single-CPU: cli/sti is sufficient to
 * serialise the rendezvous against the preempting timer IRQ. */
static inline uint32_t ipc_irq_save(void)
{
    uint32_t f;
    __asm__ volatile("pushf\n\tpop %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}
static inline void ipc_irq_restore(uint32_t f)
{
    __asm__ volatile("push %0\n\tpopf" :: "r"(f) : "memory", "cc");
}

/* Block the current task until a peer sets it back to TASK_READY.  Must be
 * called with the descriptor bookkeeping already done; the scheduler skips
 * any non-READY task, so simply marking ourselves BLOCKED and yielding parks
 * us until the rendezvous partner wakes us. */
static void ipc_block(task_t *cur)
{
    cur->state = TASK_BLOCKED;
    task_yield();     /* returns once the partner flips us back to READY */
}

void ipc_task_init(struct task *t)
{
    task_t *tt = (task_t *)t;
    tt->ipc_state    = IPC_STATE_IDLE;
    tt->ipc_partner  = 0;
    tt->ipc_rc       = 0;
    tt->ipc_sender_q = NULL;
    tt->ipc_sq_next  = NULL;
}

void ipc_task_cleanup(struct task *t)
{
    task_t  *tt = (task_t *)t;
    uint32_t fl = ipc_irq_save();

    /* Wake every sender still blocked on us; their send fails with -ESRCH. */
    task_t *s = tt->ipc_sender_q;
    while (s) {
        task_t *nx = s->ipc_sq_next;
        s->ipc_sq_next = NULL;
        s->ipc_rc      = IPC_ESRCH;
        s->ipc_state   = IPC_STATE_IDLE;
        if (s->state == TASK_BLOCKED)
            s->state = TASK_READY;
        s = nx;
    }
    tt->ipc_sender_q = NULL;

    /* If we were ourselves blocked sending, unlink from the destination's
     * queue so it never dereferences a freed slot. */
    if (tt->ipc_state == IPC_STATE_SENDING) {
        task_t *d = task_by_pid(tt->ipc_partner);
        if (d) {
            task_t *prev = NULL, *p = d->ipc_sender_q;
            while (p) {
                if (p == tt) {
                    if (prev) prev->ipc_sq_next = p->ipc_sq_next;
                    else      d->ipc_sender_q   = p->ipc_sq_next;
                    break;
                }
                prev = p;
                p = p->ipc_sq_next;
            }
        }
    }
    tt->ipc_state   = IPC_STATE_IDLE;
    tt->ipc_sq_next = NULL;

    ipc_irq_restore(fl);
}

int ipc_send(int dst, const ipc_msg_t *msg)
{
    task_t *cur = task_current();
    if (!cur)  return IPC_EINVAL;
    if (!msg)  return IPC_EFAULT;
    if (dst == cur->pid) return IPC_EINVAL;     /* no self-send */

    /* Stage the message in our own kernel buffer (valid in our context). */
    memcpy(&cur->ipc_buf, msg, sizeof(ipc_msg_t));
    cur->ipc_buf.src = cur->pid;

    uint32_t fl = ipc_irq_save();

    task_t *d = task_by_pid(dst);
    if (!d || d->state == TASK_DEAD || d->state == TASK_ZOMBIE) {
        ipc_irq_restore(fl);
        return IPC_ESRCH;
    }

    /* Fast path: the destination is already waiting to receive from us (or
     * from anyone).  Hand the message over and let it run; we don't block. */
    if (d->ipc_state == IPC_STATE_RECVING &&
        (d->ipc_partner == IPC_ANY || d->ipc_partner == cur->pid)) {
        memcpy(&d->ipc_buf, &cur->ipc_buf, sizeof(ipc_msg_t));
        d->ipc_state   = IPC_STATE_IDLE;
        d->ipc_partner = 0;
        d->ipc_rc      = 0;
        if (d->state == TASK_BLOCKED)
            d->state = TASK_READY;
        ipc_irq_restore(fl);
        return 0;
    }

    /* Slow path: enqueue on the destination's sender queue and block. */
    cur->ipc_state   = IPC_STATE_SENDING;
    cur->ipc_partner = dst;
    cur->ipc_rc      = 0;
    cur->ipc_sq_next = d->ipc_sender_q;
    d->ipc_sender_q  = cur;

    ipc_block(cur);                 /* sleep until a receiver collects us */

    int rc = cur->ipc_rc;           /* 0, or -ESRCH if the dest died first */
    cur->ipc_state = IPC_STATE_IDLE;
    ipc_irq_restore(fl);
    return rc;
}

int ipc_recv(int from, ipc_msg_t *out)
{
    task_t *cur = task_current();
    if (!cur)  return IPC_EINVAL;
    if (!out)  return IPC_EFAULT;

    uint32_t fl = ipc_irq_save();

    /* Is a matching sender already queued on us? */
    task_t *prev = NULL, *s = cur->ipc_sender_q;
    while (s) {
        if (from == IPC_ANY || s->pid == from)
            break;
        prev = s;
        s = s->ipc_sq_next;
    }

    if (s) {
        /* Dequeue the sender, take its message, complete its send. */
        if (prev) prev->ipc_sq_next = s->ipc_sq_next;
        else      cur->ipc_sender_q = s->ipc_sq_next;
        s->ipc_sq_next = NULL;

        memcpy(&cur->ipc_buf, &s->ipc_buf, sizeof(ipc_msg_t));

        s->ipc_rc      = 0;
        s->ipc_state   = IPC_STATE_IDLE;
        s->ipc_partner = 0;
        if (s->state == TASK_BLOCKED)
            s->state = TASK_READY;

        ipc_irq_restore(fl);
        memcpy(out, &cur->ipc_buf, sizeof(ipc_msg_t));   /* our context */
        return 0;
    }

    /* No sender yet: block until one delivers into our staging buffer. */
    cur->ipc_state   = IPC_STATE_RECVING;
    cur->ipc_partner = from;
    cur->ipc_rc      = 0;

    ipc_block(cur);

    int rc = cur->ipc_rc;
    cur->ipc_state = IPC_STATE_IDLE;
    ipc_irq_restore(fl);

    if (rc == 0)
        memcpy(out, &cur->ipc_buf, sizeof(ipc_msg_t));
    return rc;
}

int ipc_sendrec(int dst, ipc_msg_t *msg)
{
    int rc = ipc_send(dst, msg);
    if (rc != 0)
        return rc;
    /* Await the reply specifically from dst, overwriting the request. */
    return ipc_recv(dst, msg);
}

int ipc_nbrecv(int from, ipc_msg_t *out)
{
    task_t *cur = task_current();
    if (!cur)  return IPC_EINVAL;
    if (!out)  return IPC_EFAULT;

    uint32_t fl = ipc_irq_save();

    /* Identical match scan to ipc_recv's fast path -- but if nothing is queued
     * we return -EAGAIN instead of blocking, so a polling server stays live. */
    task_t *prev = NULL, *s = cur->ipc_sender_q;
    while (s) {
        if (from == IPC_ANY || s->pid == from)
            break;
        prev = s;
        s = s->ipc_sq_next;
    }
    if (!s) {
        ipc_irq_restore(fl);
        return IPC_EAGAIN;
    }

    if (prev) prev->ipc_sq_next = s->ipc_sq_next;
    else      cur->ipc_sender_q = s->ipc_sq_next;
    s->ipc_sq_next = NULL;

    memcpy(&cur->ipc_buf, &s->ipc_buf, sizeof(ipc_msg_t));

    s->ipc_rc      = 0;
    s->ipc_state   = IPC_STATE_IDLE;
    s->ipc_partner = 0;
    if (s->state == TASK_BLOCKED)
        s->state = TASK_READY;

    ipc_irq_restore(fl);
    memcpy(out, &cur->ipc_buf, sizeof(ipc_msg_t));   /* our context */
    return 0;
}
