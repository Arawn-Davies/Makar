/*
 * forktest.elf -- ring-3 verifier for SYS_FORK + COW (slice 12d).
 *
 * Exercises the fork path end-to-end:
 *   - parent calls sys_fork(); child gets 0, parent gets child pid
 *   - child reads a sentinel value from a writable global (must match
 *     parent's pre-fork value -- proves the page was visible via COW)
 *   - child writes a new value to the same global, then exits
 *   - parent yields a few times to let the child run, then prints its
 *     own view of the sentinel: must still be the original value
 *     (proves the COW copy gave parent a private frame on the child's
 *     write fault, OR that the page got copied for the child -- either
 *     way the two views diverge as POSIX requires)
 *
 * All output goes to fd 2 (VGA + serial) so ui-test can grep both.
 * The markers PARENT-PRE / CHILD-SAW / CHILD-WROTE / PARENT-POST and
 * the literal sentinel values are the assertion surface.
 */

#include "syscall.h"

static void write_str(int fd, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    sys_write(fd, s, (unsigned int)n);
}

static void write_hex(int fd, unsigned int v)
{
    static const char H[] = "0123456789ABCDEF";
    char buf[10];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = H[(v >> (28 - 4 * i)) & 0xF];
    sys_write(fd, buf, 10);
}

static void write_dec(int fd, int v)
{
    char buf[12];
    int i = 0;
    int neg = 0;
    unsigned int u;
    if (v < 0) { neg = 1; u = (unsigned int)(-v); }
    else       { u = (unsigned int)v; }
    if (u == 0) buf[i++] = '0';
    while (u > 0) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) sys_write(fd, "-", 1);
    while (i--) sys_write(fd, &buf[i], 1);
}

/* Sentinel lives in BSS (writable, gets COW-tagged on fork). */
static volatile unsigned int sentinel = 0xAA550000u;

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    write_str(2, "[forktest] PARENT-PRE sentinel=");
    write_hex(2, sentinel);
    write_str(2, "\n");

    int pid = sys_fork();
    if (pid < 0) {
        write_str(2, "[forktest] FORK-FAILED rc=");
        write_dec(2, pid);
        write_str(2, "\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* CHILD: read shared sentinel (proves COW visibility), then
         * write a new value (triggers the COW fault, gives child its
         * own private frame). */
        write_str(2, "[forktest] CHILD-SAW sentinel=");
        write_hex(2, sentinel);
        write_str(2, "\n");

        sentinel = 0xC0DEBABEu;

        write_str(2, "[forktest] CHILD-WROTE sentinel=");
        write_hex(2, sentinel);
        write_str(2, "\n");
        sys_exit(0);
    }

    /* PARENT: yield a few times so the child can run + exit before we
     * sample.  No wait(2) yet; this is good enough for the COW proof. */
    for (int i = 0; i < 32; i++) sys_yield();

    write_str(2, "[forktest] PARENT-POST child_pid=");
    write_dec(2, pid);
    write_str(2, " sentinel=");
    write_hex(2, sentinel);
    write_str(2, "\n");
    sys_exit(0);
    return 0;
}
