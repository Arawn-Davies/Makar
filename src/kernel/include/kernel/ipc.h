#ifndef _KERNEL_IPC_H
#define _KERNEL_IPC_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Synchronous message-passing IPC -- the microkernel primitive.
 *
 * MINIX-style rendezvous: endpoints are task PIDs, messages are fixed-size,
 * and transfer is synchronous.  A send blocks until the destination is in a
 * matching receive (and vice-versa); at the rendezvous the kernel copies the
 * message directly between the two tasks.  sendrec is an atomic
 * send-then-await-reply for request/response RPC to a server task -- the shape
 * every user-space driver/filesystem server will be driven through.
 *
 * Cross-address-space safety: each task owns a kernel-resident staging buffer
 * (task_t.ipc_buf).  A caller's user/kernel message is copied into its own
 * staging buffer in its own context; the rendezvous copy is kernel->kernel
 * (always reachable); the receiver copies out in its own context.  So the
 * sender and receiver never touch each other's address space.
 * ------------------------------------------------------------------------- */

#define IPC_MSG_DATA_WORDS 6

typedef struct {
    int32_t  src;     /* sender pid; filled in by the kernel on receive */
    int32_t  type;    /* caller-defined opcode                          */
    uint32_t data[IPC_MSG_DATA_WORDS];
} ipc_msg_t;          /* 32 bytes */

#define IPC_ANY  (-1)              /* recv-from-anyone wildcard */

/* task_t.ipc_state values */
#define IPC_STATE_IDLE     0
#define IPC_STATE_SENDING  1
#define IPC_STATE_RECVING  2

struct task;  /* forward decl -- avoids a task.h <-> ipc.h cycle */

/* Reset a task's IPC state.  Called from task_create. */
void ipc_task_init(struct task *t);

/* Detach a dying task from the IPC graph: wake any senders blocked on it
 * (handing them -ESRCH) and unlink it from a receiver's sender queue if it
 * was itself blocked sending.  Called from task_terminate. */
void ipc_task_cleanup(struct task *t);

/* Kernel-callable IPC.  `msg`/`out` must be valid in the *calling* task's
 * current address space (a ring-3 caller passes a user pointer; a kernel
 * task passes a kernel pointer).  Return 0 on success, negative on error. */
int ipc_send   (int dst,  const ipc_msg_t *msg);
int ipc_recv   (int from, ipc_msg_t *out);
int ipc_sendrec(int dst,  ipc_msg_t *msg);   /* in: request -> out: reply */

/* Non-blocking receive: deliver a queued message from `from` (or anyone) if one
 * is already waiting, else return -EAGAIN immediately without blocking.  Lets a
 * server (e.g. the makx display server) drain client requests in the same loop
 * it polls hardware input, instead of parking in a blocking ipc_recv. */
int ipc_nbrecv (int from, ipc_msg_t *out);

#endif /* _KERNEL_IPC_H */
