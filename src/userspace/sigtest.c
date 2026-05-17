/*
 * sigtest.elf - ring-3 verifier for slice 8 phase 4 (user signal handlers).
 *
 * Installs a SIGUSR1 handler, sends itself SIGUSR1, yields a few times so
 * the kernel's "return to ring 3 with pending signal" path fires, then
 * inspects a handler-set flag and prints either:
 *
 *     sigtest: SIGUSR1 handler ran (count=N)
 *     sigtest: SIGUSR1 handler NEVER ran
 *
 * via SYS_WRITE_SERIAL so ui_test / iso-test can grep it.  Pairs with
 * the kernel-side sigframe + trampoline + SYS_SIGRETURN landed in this
 * same commit.
 */

#include "syscall.h"

static volatile int g_signal_count = 0;
static volatile int g_last_signo   = 0;

static void diag(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    sys_write_serial(s, n);
    sys_write(2, s, n);
}

static void usr1_handler(int signo)
{
    g_signal_count++;
    g_last_signo = signo;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    diag("sigtest: installing SIGUSR1 handler\n");

    sig_handler_t prev = sys_signal(SIGUSR1, usr1_handler);
    if ((unsigned long)prev == (unsigned long)-1) {
        diag("sigtest: sys_signal failed\n");
        return 1;
    }

    /* Self-signal.  The kernel posts the pending bit; on the iret back
     * to ring 3 from this very syscall, signal_check_user builds a
     * sigframe and our handler runs before main resumes. */
    diag("sigtest: sending SIGUSR1 to self\n");

    /* Read our own pid via /proc/self?  We don't have getpid() yet, but
     * we can pass pid=0 to mean "self" -- except the kernel's SYS_KILL
     * resolves by exact pid match.  So we need the real pid.  Quickest
     * path: scrape /proc/tasks for the RUN entry, the only running task
     * at this moment is us. */
    /* Simpler: use SYS_GETKEY?  No.  Just call sys_kill with our pid
     * obtained from a different angle.  Without getpid() the easiest
     * route is to have the kernel send SIGUSR1 to "the current task" --
     * which sys_kill doesn't expose.  Use sys_signal to install the
     * handler, then *the shell* would send the signal in a real test.
     *
     * For an isolated self-test, we approximate by reading /proc/tasks
     * and finding the line whose state is RUN. */

    char buf[1024];
    int n = sys_ls_dir("/proc", buf, sizeof(buf) - 1);
    (void)n;
    /* Reading /proc directory listing isn't what we want -- we need the
     * tasks file contents.  Open it instead. */
    int fd = sys_open("/proc/tasks", O_RDONLY);
    if (fd < 0) {
        diag("sigtest: cannot open /proc/tasks\n");
        return 2;
    }
    long got = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (got <= 0) {
        diag("sigtest: /proc/tasks empty\n");
        return 3;
    }
    buf[got] = '\0';

    /* Find the "RUN " token; the PID at the start of that line is ours. */
    int my_pid = -1;
    for (long i = 0; i + 4 < got; i++) {
        if (buf[i] == 'R' && buf[i+1] == 'U' && buf[i+2] == 'N'
                          && buf[i+3] == ' ') {
            /* Walk back to start of line. */
            long ls = i;
            while (ls > 0 && buf[ls - 1] != '\n') ls--;
            /* Parse leading decimal. */
            int v = 0;
            while (buf[ls] >= '0' && buf[ls] <= '9') {
                v = v * 10 + (buf[ls] - '0');
                ls++;
            }
            my_pid = v;
            break;
        }
    }
    if (my_pid <= 0) {
        diag("sigtest: cannot determine self pid\n");
        return 4;
    }

    if (sys_kill(my_pid, SIGUSR1) != 0) {
        diag("sigtest: sys_kill failed\n");
        return 5;
    }

    /* Yield a couple of times so any kernel-side delivery work
     * definitely runs.  In practice signal_check_user fires on the
     * iret back from sys_kill itself, so by the time sys_kill returns
     * the handler has already run -- the yields are belt-and-braces
     * for any future kernel routing change. */
    sys_yield();
    sys_yield();

    if (g_signal_count > 0 && g_last_signo == SIGUSR1) {
        diag("sigtest: SIGUSR1 handler ran (count=");
        /* Print decimal count without a libc -- single digit suffices
         * since we only fired once. */
        char d[2] = { (char)('0' + (g_signal_count % 10)), 0 };
        diag(d);
        diag(")\n");
        return 0;
    }

    diag("sigtest: SIGUSR1 handler NEVER ran\n");
    return 6;
}
