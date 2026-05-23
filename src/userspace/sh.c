/*
 * sh.c -- ring-3 userspace shell, MVP.
 *
 * First concrete step toward lifting the shell out of the kernel: a
 * freestanding C program that reads commands from fd 0, dispatches
 * builtins inline, and forks+execve's external commands.  Coexists with
 * the in-kernel shell -- run it with `exec /apps/sh.elf` from any kernel
 * shell prompt.  Exit (Ctrl-D, `exit`, or wait4 failure) returns to the
 * parent shell.
 *
 * Deliberately Spartan to stay TCC-rebuildable in-OS (see
 * test_tcc_rebuild_sh): only depends on syscall.h, no libc shim, no
 * GCC-isms.  This is the same self-host story as calc.c.
 *
 * Not yet implemented (followups, in order):
 *   - $VAR / ${VAR} expansion, quotes, escapes
 *   - PATH lookup (argv[0] must be a path)
 *   - Pipes (|), redirection (<, >, >>), job control (&)
 *   - Signal forwarding to children
 *   - Subshells, command substitution
 */
#include "syscall.h"

#define LINE_MAX  512
#define MAX_ARGS  16

/* ---------- string helpers (no libc) ---------- */

static unsigned int s_len(const char *s)
{
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void put_s(const char *s)
{
    sys_write(1, s, s_len(s));
}

static void put_c(char c)
{
    sys_write(1, &c, 1);
}

/* atoi for the optional `exit N` arg.  Returns 0 on empty/garbage. */
static int s_atoi(const char *s)
{
    int sign = 1;
    int v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

/* ---------- tokenizer ---------- */

/* Split `line` in-place on ASCII whitespace.  Writes pointer-to-token
 * into argv[0..]; appends NULL.  Returns argc. */
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        /* advance to end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = (char *)0;
    return argc;
}

/* ---------- builtins ---------- */

/* Returns 1 if `argv[0]` was a builtin (and was handled), 0 otherwise.
 * Sets *exit_status when the shell itself should terminate (exit builtin). */
static int run_builtin(int argc, char **argv, int *should_exit, int *exit_status)
{
    if (argc == 0) return 1;  /* empty line: handled (no-op) */

    if (s_eq(argv[0], "exit")) {
        *should_exit = 1;
        *exit_status = (argc > 1) ? s_atoi(argv[1]) : 0;
        return 1;
    }
    if (s_eq(argv[0], "cd")) {
        const char *target = (argc > 1) ? argv[1] : "/";
        if (sys_chdir(target) != 0) {
            put_s("cd: ");
            put_s(target);
            put_s(": no such directory\n");
        }
        return 1;
    }
    if (s_eq(argv[0], "pwd")) {
        char buf[256];
        int n = sys_getcwd(buf, sizeof(buf));
        if (n < 0) put_s("pwd: error\n");
        else { put_s(buf); put_c('\n'); }
        return 1;
    }
    return 0;
}

/* ---------- external command dispatch ---------- */

static void run_external(int argc, char **argv)
{
    (void)argc;
    int pid = sys_fork();
    if (pid < 0) {
        put_s("sh: fork failed\n");
        return;
    }
    if (pid == 0) {
        /* child */
        sys_execve(argv[0], argv, (char *const *)0);
        /* execve only returns on failure */
        put_s("sh: ");
        put_s(argv[0]);
        put_s(": command not found\n");
        sys_exit(127);
    }
    /* parent: wait */
    int status = 0;
    sys_wait4(pid, &status, 0);
}

/* ---------- main REPL ---------- */

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    put_s("sh.elf: ring-3 userspace shell (Ctrl-D or `exit` to quit)\n");

    char line[LINE_MAX];
    char *args[MAX_ARGS];

    for (;;) {
        put_s("$ ");
        long n = sys_read(0, line, sizeof(line) - 1);
        if (n <= 0) {           /* EOF / Ctrl-D / error */
            put_c('\n');
            break;
        }
        if (line[n - 1] == '\n') n--;
        line[n] = '\0';

        int ac = tokenize(line, args);
        if (ac == 0) continue;

        int should_exit = 0;
        int exit_status = 0;
        if (run_builtin(ac, args, &should_exit, &exit_status)) {
            if (should_exit) return exit_status;
            continue;
        }
        run_external(ac, args);
    }

    return 0;
}
