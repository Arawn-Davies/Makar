#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>
#include <kernel/isr.h>

/* The syscall ABI is shared verbatim with userspace (Linux-uapi style):
 *   <makar_syscalls.h> -- SYS_* numbers
 *   <makar_abi.h>      -- clockids, struct timeval/timespec/stat/dirent,
 *                         tty_cell_t, open/fcntl flags, S_IF + DT_ bits
 * Only kernel-internal bits + the syscall_init/dispatch decls stay here. */
#include <makar_syscalls.h>
#include <makar_abi.h>

/* Well-known file descriptors. */
#define FD_STDIN    0
#define FD_STDOUT   1
#define FD_STDERR   2

/* Maximum size of a regular file backed by an in-memory FD_KIND_FILE buffer.
 * Reads cap the eager-load here; writes may grow the buffer up to this hard
 * limit (16 MiB) before SYS_WRITE returns -1 (EFBIG).  Kernel heap is 32 MiB
 * (see HEAP_MAX in kernel/heap.h), so two simultaneously-open large files
 * still leave the kernel room to operate.  16 MiB is enough for tcc.c + its
 * emitted ELF on a self-compile attempt. */
#define SYSCALL_FILE_MAX     (16u * 1024u * 1024u)
/* Initial heap allocation for a freshly-created (O_CREAT) or O_TRUNC'd fd.
 * Subsequent SYS_WRITEs grow geometrically (doubling). */
#define SYSCALL_FILE_INITIAL (4u * 1024u)

/*
 * g_ring3_last_cp - last SYS_DEBUG checkpoint value received from ring-3.
 * Reset to 0 before launching a ring-3 test task; read after it exits.
 */
extern volatile uint32_t g_ring3_last_cp;

/*
 * syscall_init - register int 0x80 in the interrupt-handler table.
 *
 * Must be called after init_descriptor_tables() and tasking_init().
 */
void syscall_init(void);

/*
 * syscall_dispatch - the int 0x80 C handler; exposed for in-kernel testing.
 */
void syscall_dispatch(registers_t *regs);

#endif /* _KERNEL_SYSCALL_H */
