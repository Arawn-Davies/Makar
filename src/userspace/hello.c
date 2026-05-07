#include "syscall.h"

static void write_str(const char *s)
{
    unsigned int len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

int main(int argc, char **argv, char **envp)
{
    /* ---------------------------------------------------------------------
     * Argument handling — POSIX / SUSv4 (XSI) convention.
     *
     * The signature `int main(int argc, char **argv, char **envp)` is the
     * standard hosted-environment entry point defined by ISO C (§5.1.2.2.1)
     * and refined by POSIX.1-2017 (XBD §8). Three guarantees apply:
     *
     *   1. argv[0]            is the program invocation name (e.g. "hello").
     *   2. argv[1] .. argv[argc-1]  are the actual user-supplied arguments.
     *   3. argv[argc]         is a NULL pointer (sentinel).
     *
     * So when the Makar shell runs:
     *
     *     exec hello tester
     *
     * the child sees:
     *
     *     argc = 2
     *     argv = { "hello", "tester", NULL }
     *
     * argv[0] is *not* the data — it's metadata identifying the program for
     * tools like ps(1), getopt(3), and any "Usage:" output. The first real
     * argument is therefore at argv[1]. This is why we don't read argv[0]:
     * doing so would "greet" the program's own name when called bare, which
     * is both a POSIX violation and a UX bug.
     *
     * Makar's userspace is held to this convention end-to-end (crt0.S → ELF
     * loader → shell `exec` builtin → here) so that any future libc port
     * (musl, uClibc-ng, etc.) drops in with no glue.
     *
     * Disclaimer: Makar is not, in fact, a UNIX® — that mark belongs to The
     * Open Group and requires passing the SUS test suite plus a cheque we
     * are not in a position to write. Spiritually, however, it is one. The
     * code in this file would pass; the paperwork would not.
     * ------------------------------------------------------------------- */
    if (argc >= 2 && argv[1] && argv[1][0]) {
        write_str("Hello, ");
        write_str(argv[1]);
        write_str("!\n");
        return 0;
    }

    char name[64];
    write_str("What is your name? ");

    long n = sys_read(0, name, sizeof(name) - 1);
    if (n > 0) {
        if (name[n - 1] == '\n')
            n--;
        name[n] = '\0';
    } else {
        name[0] = '\0';
    }

    write_str("Hello, ");
    write_str(name[0] ? name : "stranger");
    write_str("!\n");

    return 0;
}
