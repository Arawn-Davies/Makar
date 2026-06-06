#ifndef _MAKAR_SIGNALS_H
#define _MAKAR_SIGNALS_H

/*
 * makar_signals.h -- canonical Linux i386 signal numbers, shared by the kernel
 * (<kernel/signal.h>) and userspace (src/userspace/syscall.h).  Was kept in sync
 * by hand.  Numbers only; the kernel keeps SIG_MAX/SIG_BIT + the handler table.
 */
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

#endif /* _MAKAR_SIGNALS_H */
