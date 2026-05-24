/*
 * sh.c -- ring-3 userspace shell, MVP + readline/history (slice 20b).
 *
 * First concrete step toward lifting the shell out of the kernel: a
 * freestanding C program that reads commands one keystroke at a time
 * via sys_getkey() (which already delivers KEY_ARROW_* sentinels in
 * the default kernel translation -- "raw mode" is the kbtester surface
 * and intentionally not used here, since it'd leak modifier-event bytes
 * through to any forked child), dispatches builtins inline, and
 * forks+execve's external commands.  Coexists with the in-kernel
 * shell -- run it with `exec /apps/sh.elf` from any kernel shell prompt.
 * Exit (Ctrl-D on empty line, `exit`, or wait4 failure) returns to the
 * parent shell.
 *
 * Deliberately Spartan to stay TCC-rebuildable in-OS (see
 * test_tcc_rebuild_sh): only depends on syscall.h, no libc shim, no
 * GCC-isms.  Mirrors the in-kernel shell's inline-edit + 16-entry
 * history surface; the kernel implementation lives in
 * src/kernel/arch/i386/shell/shell.c:shell_readline.
 *
 * Not yet implemented (followups, in order):
 *   - $VAR / ${VAR} expansion, quotes, escapes                 (slice 20c)
 *   - Control flow: if/while/for/[ TEST ]                      (slice 20d)
 *   - Tab + glob completion                                    (slice 20e)
 *   - PATH lookup (argv[0] must be a path)
 *   - Pipes (|), redirection (<, >, >>), job control (&)       (slice 28)
 *   - Signal forwarding to children
 *   - Subshells, command substitution
 */
#include "syscall.h"

#define LINE_MAX  512
#define MAX_ARGS  16
#define HIST_MAX  16

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

/* ---------- history (ring buffer, newest-first navigation) ---------- */

static char         g_hist[HIST_MAX][LINE_MAX];
static unsigned int g_hist_count = 0;   /* 0..HIST_MAX             */
static unsigned int g_hist_head  = 0;   /* index of next insertion */

/* hist_get(i): i=0 newest, i=g_hist_count-1 oldest, NULL out-of-range. */
static const char *hist_get(unsigned int i)
{
    if (i >= g_hist_count) return (const char *)0;
    unsigned int idx = (g_hist_head + HIST_MAX - 1 - i) % HIST_MAX;
    return g_hist[idx];
}

static void hist_push(const char *s)
{
    unsigned int n = s_len(s);
    if (n == 0) return;
    /* Skip immediate duplicate (so ↑ doesn't show the same line twice). */
    const char *latest = hist_get(0);
    if (latest && s_eq(latest, s)) return;
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    char *slot = g_hist[g_hist_head];
    for (unsigned int i = 0; i < n; i++) slot[i] = s[i];
    slot[n] = '\0';
    g_hist_head = (g_hist_head + 1) % HIST_MAX;
    if (g_hist_count < HIST_MAX) g_hist_count++;
}

/* ---------- inline-edit readline ---------- */

/* readline: print prompt, edit a line in raw mode, return its length
 * (NUL-terminated in `buf`).  Returns -1 on Ctrl-D at empty line
 * (caller treats as EOF) or -2 on Ctrl-C (caller treats as aborted
 * line -- discard and reprompt). */
static int readline(const char *prompt, char *buf)
{
    put_s(prompt);

    unsigned int len = 0;
    unsigned int cur = 0;
    int          hist_idx = -1;      /* -1 = editing fresh buffer */
    char         saved[LINE_MAX];    /* snapshot before history nav */
    saved[0] = '\0';
    unsigned int saved_len = 0;

    for (;;) {
        int c = sys_getkey();
        if (c < 0) continue;
        unsigned char ch = (unsigned char)c;

        if (ch == '\n' || ch == '\r') {
            put_c('\n');
            buf[len] = '\0';
            return (int)len;
        }
        if (ch == 0x03) {                       /* Ctrl-C */
            put_s("^C\n");
            buf[0] = '\0';
            return -2;
        }
        if (ch == 0x04) {                       /* Ctrl-D */
            if (len == 0) return -1;
            continue;
        }
        if (ch == 0x08 || ch == 0x7F) {         /* Backspace */
            if (cur == 0) continue;
            /* Delete char before cursor; shift tail left. */
            for (unsigned int i = cur - 1; i + 1 < len; i++) buf[i] = buf[i + 1];
            len--;
            cur--;
            /* Visual: back one, redraw tail + space, back (len-cur+1). */
            put_c('\b');
            if (len > cur) sys_write(1, &buf[cur], len - cur);
            put_c(' ');
            for (unsigned int i = 0; i <= len - cur; i++) put_c('\b');
            continue;
        }
        if (ch == KEY_ARROW_LEFT) {
            if (cur > 0) { put_c('\b'); cur--; }
            continue;
        }
        if (ch == KEY_ARROW_RIGHT) {
            if (cur < len) { put_c(buf[cur]); cur++; }
            continue;
        }
        if (ch == KEY_ARROW_UP || ch == KEY_ARROW_DOWN) {
            int new_idx;
            if (ch == KEY_ARROW_UP) {
                new_idx = (hist_idx < 0) ? 0 : hist_idx + 1;
                if ((unsigned int)new_idx >= g_hist_count) continue;
                if (hist_idx < 0) {
                    /* Leaving fresh buffer: snapshot it. */
                    for (unsigned int i = 0; i < len; i++) saved[i] = buf[i];
                    saved[len] = '\0';
                    saved_len = len;
                }
            } else {
                if (hist_idx < 0) continue;     /* already at newest/fresh */
                new_idx = hist_idx - 1;
            }
            /* Wipe current text: back to start, overwrite with spaces, back. */
            for (unsigned int i = 0; i < cur; i++) put_c('\b');
            for (unsigned int i = 0; i < len; i++) put_c(' ');
            for (unsigned int i = 0; i < len; i++) put_c('\b');
            /* Load new contents from history or saved buffer. */
            const char *src;
            unsigned int n;
            if (new_idx < 0) { src = saved; n = saved_len; }
            else {
                src = hist_get((unsigned int)new_idx);
                n = src ? s_len(src) : 0;
            }
            if (n >= LINE_MAX) n = LINE_MAX - 1;
            for (unsigned int i = 0; i < n; i++) buf[i] = src[i];
            buf[n] = '\0';
            len = n;
            cur = n;
            if (len > 0) sys_write(1, buf, len);
            hist_idx = new_idx;
            continue;
        }
        /* Printable ASCII only -- drop other control bytes + sentinels. */
        if (ch < 0x20 || ch >= 0x80) continue;
        if (len >= LINE_MAX - 1) continue;
        /* Insert at cursor: shift tail right, write, redraw tail. */
        for (unsigned int i = len; i > cur; i--) buf[i] = buf[i - 1];
        buf[cur] = (char)ch;
        len++;
        cur++;
        sys_write(1, &buf[cur - 1], len - (cur - 1));
        for (unsigned int i = 0; i < len - cur; i++) put_c('\b');
    }
}

/* ---------- tokenizer ---------- */

/* Split `line` in-place on ASCII whitespace.  Writes pointer-to-token
 * into argv[0..]; appends NULL.  Returns argc. */
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
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
    char dispatch[LINE_MAX];        /* tokenize() destroys its input,
                                       so we keep `line` pristine for
                                       history_push().                  */
    char *args[MAX_ARGS];

    for (;;) {
        int n = readline("$ ", line);
        if (n == -1) {              /* EOF / Ctrl-D on empty line */
            put_c('\n');
            break;
        }
        if (n == -2) continue;      /* Ctrl-C: abort line, reprompt */

        if (n > 0) hist_push(line);

        /* Copy line into a scratch buffer for the destructive tokenize. */
        int i;
        for (i = 0; i < n && i < LINE_MAX - 1; i++) dispatch[i] = line[i];
        dispatch[i] = '\0';

        int ac = tokenize(dispatch, args);
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
