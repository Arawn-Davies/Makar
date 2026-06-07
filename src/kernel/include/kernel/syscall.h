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

/* Hard cap on a *writable* FD_KIND_FILE's growable heap buffer before SYS_WRITE
 * returns -1 (EFBIG).  Read-only files are no longer eager-loaded -- they stream
 * lazily through the page cache (see fd.h / SYS_OPEN) -- so this only bounds the
 * write path now, and the big-IWAD band-aid (raised to 32 MiB by commit d530731
 * to fit FreeDOOM in one eager buffer) is reverted to the original 8 MiB. */
#define SYSCALL_FILE_MAX     (8u * 1024u * 1024u)
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
