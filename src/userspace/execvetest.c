/*
 * execvetest.elf -- ring-3 verifier for SYS_EXECVE (slice 13a).
 *
 * Flow:
 *   1. parent prints PRE-EXEC marker so we can see it before fork
 *   2. fork() -- child gets 0, parent gets child pid
 *   3. CHILD execve's /cdrom/apps/hello.elf with argv = {"hello", "execve-tester"}
 *      -- on success this never returns; control resumes in hello.elf's
 *      _start with argc/argv on the user stack
 *      -- hello.elf's argv[1]-path prints "Hello, execve-tester!\n" and
 *      exits 0, which closes the child task
 *   4. PARENT yields a few times, then prints POST-EXEC marker
 *      -- proves parent's address space was unaffected by child's execve
 *
 * The serial assertion surface:
 *   "[execve-test] PRE-EXEC"
 *   "Hello, execve-tester!"   (from hello.elf inside child)
 *   "[execve-test] POST-EXEC" (from parent, post-child)
 */

#include "syscall.h"

static void write_str(int fd, const char *s)
{
    int n = 0;
    while (s[n]) n++;
    sys_write(fd, s, (unsigned int)n);
}

static void write_dec(int fd, int v)
{
    char buf[12];
    int i = 0;
    unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
    if (v < 0) sys_write(fd, "-", 1);
    if (u == 0) buf[i++] = '0';
    while (u > 0) { buf[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i--) sys_write(fd, &buf[i], 1);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    write_str(2, "[execve-test] PRE-EXEC\n");

    int pid = sys_fork();
    if (pid < 0) {
        write_str(2, "[execve-test] FORK-FAILED rc=");
        write_dec(2, pid);
        write_str(2, "\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* CHILD: replace ourselves with hello.elf.  argv[0] is the
         * program name (hello reads only argv[1]); argv[1] is the
         * recognisable marker we expect hello to greet by name. */
        char *cargv[] = { "hello", "execve-tester", 0 };
        int rc = sys_execve("/cdrom/apps/hello.elf", cargv, 0);

        /* Only reached if execve failed; print the negative errno. */
        write_str(2, "[execve-test] CHILD-EXECVE-FAILED rc=");
        write_dec(2, rc);
        write_str(2, "\n");
        sys_exit(2);
    }

    /* PARENT: yield until child has had a chance to load + run hello +
     * exit.  Without wait4(2), this is the best we can do. */
    for (int i = 0; i < 96; i++) sys_yield();

    write_str(2, "[execve-test] POST-EXEC parent_pid_alive child_was=");
    write_dec(2, pid);
    write_str(2, "\n");
    sys_exit(0);
    return 0;
}
