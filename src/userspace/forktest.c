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

/* Single-page sentinel (12d default). */
static volatile unsigned int sentinel = 0xAA550000u;

/* Multi-page sentinels (12e).  Each lives in its own page so the child's
 * writes trigger four independent COW faults; the parent's view of each
 * must remain at the original value.  Page alignment forces the linker
 * to place each on its own 4 KiB frame; the +PAGE filler is the minimum
 * needed for the next-aligned `static` to actually land in a fresh page
 * rather than sharing one with its neighbour.
 *
 * NOTE: aligned(4096) needs link.ld's SECTIONS to align .bss to 4 KiB;
 * checked by inspection -- isodir/apps/forktest.elf objdump shows the
 * four pages laid out as expected. */
#define PAGE 4096
static volatile unsigned int p1[PAGE / sizeof(unsigned int)] __attribute__((aligned(PAGE)));
static volatile unsigned int p2[PAGE / sizeof(unsigned int)] __attribute__((aligned(PAGE)));
static volatile unsigned int p3[PAGE / sizeof(unsigned int)] __attribute__((aligned(PAGE)));
static volatile unsigned int p4[PAGE / sizeof(unsigned int)] __attribute__((aligned(PAGE)));

/* Magic exit status -- ui-test asserts on the kernel's `status=42` log
 * line to confirm the child's exit code propagated through SYS_EXIT. */
#define CHILD_EXIT_STATUS 42

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* Seed the four multi-page sentinels with distinct, recognisable
     * values so a child-side stomp on one page can't accidentally pass
     * an audit on another. */
    p1[0] = 0x11110000u;
    p2[0] = 0x22220000u;
    p3[0] = 0x33330000u;
    p4[0] = 0x44440000u;

    write_str(2, "[forktest] PARENT-PRE sentinel=");
    write_hex(2, sentinel);
    write_str(2, " p1=");      write_hex(2, p1[0]);
    write_str(2, " p2=");      write_hex(2, p2[0]);
    write_str(2, " p3=");      write_hex(2, p3[0]);
    write_str(2, " p4=");      write_hex(2, p4[0]);
    write_str(2, "\n");

    int pid = sys_fork();
    if (pid < 0) {
        write_str(2, "[forktest] FORK-FAILED rc=");
        write_dec(2, pid);
        write_str(2, "\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* CHILD: read all sentinels (proves multi-page COW visibility). */
        write_str(2, "[forktest] CHILD-SAW sentinel=");
        write_hex(2, sentinel);
        write_str(2, " p1=");  write_hex(2, p1[0]);
        write_str(2, " p2=");  write_hex(2, p2[0]);
        write_str(2, " p3=");  write_hex(2, p3[0]);
        write_str(2, " p4=");  write_hex(2, p4[0]);
        write_str(2, "\n");

        /* Write each page (each triggers an independent COW fault). */
        sentinel = 0xC0DEBABEu;
        p1[0]    = 0xDEAD0001u;
        p2[0]    = 0xDEAD0002u;
        p3[0]    = 0xDEAD0003u;
        p4[0]    = 0xDEAD0004u;

        write_str(2, "[forktest] CHILD-WROTE sentinel=");
        write_hex(2, sentinel);
        write_str(2, " p1=");  write_hex(2, p1[0]);
        write_str(2, " p2=");  write_hex(2, p2[0]);
        write_str(2, " p3=");  write_hex(2, p3[0]);
        write_str(2, " p4=");  write_hex(2, p4[0]);
        write_str(2, "\n");

        /* Exit with a recognisable non-zero status. */
        sys_exit(CHILD_EXIT_STATUS);
    }

    /* PARENT: yield a few times so the child can run + exit before we
     * sample.  No wait(2) yet; this is good enough for the COW proof. */
    for (int i = 0; i < 64; i++) sys_yield();

    write_str(2, "[forktest] PARENT-POST child_pid=");
    write_dec(2, pid);
    write_str(2, " sentinel=");
    write_hex(2, sentinel);
    write_str(2, " p1=");      write_hex(2, p1[0]);
    write_str(2, " p2=");      write_hex(2, p2[0]);
    write_str(2, " p3=");      write_hex(2, p3[0]);
    write_str(2, " p4=");      write_hex(2, p4[0]);
    write_str(2, "\n");
    sys_exit(0);
    return 0;
}
