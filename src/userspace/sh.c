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
 *   - Control flow: if/while/for/[ TEST ]                      (slice 20d)
 *   - Tab + glob completion                                    (slice 20e)
 *   - PATH lookup (argv[0] must be a path)
 *   - Pipes (|), redirection (<, >, >>), job control (&)       (slice 28)
 *   - Quotes / escapes / inline NAME=VAL CMD assignments
 *   - Signal forwarding to children
 *   - Subshells, command substitution
 */
#include "syscall.h"

#define LINE_MAX      512
#define MAX_ARGS      16
#define HIST_MAX      16
#define VAR_MAX       32
#define VAR_NAME_MAX  32
#define VAR_VAL_MAX   192
#define VFS_PATH_MAX  256   /* mirrors kernel/vfs.h; sized to fit /apps/<bin>.elf */

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

/* ---------- variable table (slice 20c) ----------
 *
 * Fixed-size table of NAME=value pairs, local to this sh.elf instance.
 * Mirrors the per-VT isolation model of the kernel's task_t.script_vars:
 * each ring-3 shell has its own table, no cross-VT leakage.  No malloc
 * (TCC-rebuildable, freestanding).
 *
 * $?  -- exposed via expand() as a magic name, not stored in the table.
 *        Tracked in g_last_status, updated after every builtin and every
 *        external dispatch.
 *
 * Inline `NAME=VAL CMD` env-prefixes (bash-style transient assignments)
 * are NOT supported in this slice -- only standalone `NAME=VAL` lines.
 */

typedef struct {
    char name[VAR_NAME_MAX];
    char val[VAR_VAL_MAX];
} var_t;

static var_t        g_vars[VAR_MAX];
static unsigned int g_var_count  = 0;
static int          g_last_status = 0;

static int var_name_ok(const char *n)
{
    if (!*n) return 0;
    if (n[0] >= '0' && n[0] <= '9') return 0;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static const char *var_get(const char *name)
{
    for (unsigned int i = 0; i < g_var_count; i++)
        if (s_eq(g_vars[i].name, name)) return g_vars[i].val;
    return (const char *)0;
}

static int var_set(const char *name, const char *val)
{
    if (!var_name_ok(name)) return -1;
    /* Existing slot? Overwrite. */
    for (unsigned int i = 0; i < g_var_count; i++) {
        if (s_eq(g_vars[i].name, name)) {
            unsigned int j;
            for (j = 0; val[j] && j < VAR_VAL_MAX - 1; j++) g_vars[i].val[j] = val[j];
            g_vars[i].val[j] = '\0';
            return 0;
        }
    }
    if (g_var_count >= VAR_MAX) return -1;
    unsigned int j;
    for (j = 0; name[j] && j < VAR_NAME_MAX - 1; j++) g_vars[g_var_count].name[j] = name[j];
    g_vars[g_var_count].name[j] = '\0';
    for (j = 0; val[j] && j < VAR_VAL_MAX - 1; j++) g_vars[g_var_count].val[j] = val[j];
    g_vars[g_var_count].val[j] = '\0';
    g_var_count++;
    return 0;
}

static int var_unset(const char *name)
{
    for (unsigned int i = 0; i < g_var_count; i++) {
        if (s_eq(g_vars[i].name, name)) {
            for (unsigned int j = i; j + 1 < g_var_count; j++) g_vars[j] = g_vars[j + 1];
            g_var_count--;
            return 0;
        }
    }
    return -1;
}

/* Print decimal int to `out` (caller guarantees >= 12 bytes). */
static void status_str(char *out, int v)
{
    char tmp[12];
    int i = 0;
    int sign = (v < 0);
    unsigned int u = sign ? (unsigned int)(-v) : (unsigned int)v;
    if (u == 0) tmp[i++] = '0';
    while (u > 0) { tmp[i++] = (char)('0' + (u % 10u)); u /= 10u; }
    int k = 0;
    if (sign) out[k++] = '-';
    while (i > 0) out[k++] = tmp[--i];
    out[k] = '\0';
}

/* Expand $VAR / ${VAR} / $? in `in` -> `out` (NUL-terminated, capped at
 * outsz-1).  Unknown vars expand to empty (POSIX).  No quote handling
 * in this slice; that's a 20d/20e followup. */
static void expand(const char *in, char *out, unsigned int outsz)
{
    unsigned int o = 0;
    while (*in && o + 1 < outsz) {
        if (*in != '$') {
            out[o++] = *in++;
            continue;
        }
        in++;  /* consume '$' */
        if (*in == '?') {
            char num[12];
            status_str(num, g_last_status);
            for (unsigned int j = 0; num[j] && o + 1 < outsz; j++) out[o++] = num[j];
            in++;
            continue;
        }
        char  name[VAR_NAME_MAX];
        unsigned int n = 0;
        int   braced = 0;
        if (*in == '{') { braced = 1; in++; }
        while (*in && n + 1 < VAR_NAME_MAX) {
            char c = *in;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_';
            if (!ok) break;
            name[n++] = c;
            in++;
        }
        name[n] = '\0';
        if (braced && *in == '}') in++;
        if (n == 0) {
            /* Lone `$` -- pass through. */
            if (o + 1 < outsz) out[o++] = '$';
            continue;
        }
        const char *v = var_get(name);
        if (v) for (unsigned int j = 0; v[j] && o + 1 < outsz; j++) out[o++] = v[j];
    }
    out[o] = '\0';
}

/* Detect a standalone assignment: `NAME=...` with NAME a valid identifier.
 * Returns pointer to the `=` sign, or NULL if not an assignment. */
static const char *assign_eq(const char *line)
{
    if (!line || !*line) return (const char *)0;
    const char *p = line;
    /* First char: letter or _ */
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return (const char *)0;
    p++;
    while (*p && *p != '=' && *p != ' ' && *p != '\t') {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return (const char *)0;
        p++;
    }
    if (*p != '=') return (const char *)0;
    return p;
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
    if (s_eq(argv[0], "env")) {
        /* Dump table -- one NAME=value per line.  Mirrors the kernel
         * sh_script.c `env` builtin. */
        for (unsigned int i = 0; i < g_var_count; i++) {
            put_s(g_vars[i].name);
            put_c('=');
            put_s(g_vars[i].val);
            put_c('\n');
        }
        return 1;
    }
    if (s_eq(argv[0], "unset")) {
        for (int i = 1; i < argc; i++) var_unset(argv[i]);
        return 1;
    }
    if (s_eq(argv[0], "read")) {
        if (argc < 2) { put_s("read: missing variable name\n"); return 1; }
        if (!var_name_ok(argv[1])) { put_s("read: invalid name\n"); return 1; }
        char buf[LINE_MAX];
        int n = readline("", buf);   /* no prompt -- caller's responsibility */
        if (n < 0) { var_set(argv[1], ""); return 1; }
        var_set(argv[1], buf);
        return 1;
    }
    return 0;
}

/* ---------- external command dispatch ---------- */

/* makbox multicall applets.  Bare command names matching this list are
 * routed via /apps/makbox.elf <applet> <args...> -- same restricted
 * fallback as the kernel shell (only real applet names get the auto-
 * route, typos hit "Unknown command").  Keep in sync with shell.c's
 * MAKBOX_APPLETS table. */
static int is_makbox_applet(const char *name)
{
    static const char *applets[] = {
        "ls", "cat", "cp", "mv", "rm", "rmdir", "echo", "pwd", (const char *)0
    };
    for (int i = 0; applets[i]; i++)
        if (s_eq(name, applets[i])) return 1;
    return 0;
}

/* True if argv[0] looks like a path (absolute or relative): starts with
 * '/' or './'.  Kernel shell behaviour -- path-style argv[0]s skip both
 * the PATH walk and the makbox rewrite (the user explicitly named a
 * file; bareword fallbacks would be confusing). */
static int looks_like_path(const char *s)
{
    if (s[0] == '/') return 1;
    if (s[0] == '.' && s[1] == '/') return 1;
    return 0;
}

/* sys_stat probe: 1 if `path` resolves to a node, 0 otherwise. */
static int path_exists(const char *path)
{
    struct stat st;
    return sys_stat(path, &st) == 0;
}

/* spawn(): fork + execve + wait4, returns child status (low 7 bits). */
static int spawn(const char *path, char **argv)
{
    int pid = sys_fork();
    if (pid < 0) { put_s("sh: fork failed\n"); return 1; }
    if (pid == 0) {
        sys_execve(path, argv, (char *const *)0);
        sys_exit(127);   /* execve only returns on failure */
    }
    int status = 0;
    sys_wait4(pid, &status, 0);
    return status & 0x7F;
}

/* Try to exec at `path`; if missing, retry with ".elf" appended.
 * Returns 1 (and writes status into *out_status) if a binary was
 * actually spawned; 0 if neither path existed. */
static int try_exec_path(const char *path, char **argv, int *out_status)
{
    if (path_exists(path)) { *out_status = spawn(path, argv); return 1; }
    char with_ext[VFS_PATH_MAX];
    unsigned int plen = s_len(path);
    if (plen + 5 >= VFS_PATH_MAX) return 0;
    unsigned int i;
    for (i = 0; i < plen; i++) with_ext[i] = path[i];
    with_ext[i++] = '.'; with_ext[i++] = 'e';
    with_ext[i++] = 'l'; with_ext[i++] = 'f'; with_ext[i]   = '\0';
    if (path_exists(with_ext)) { *out_status = spawn(with_ext, argv); return 1; }
    return 0;
}

/* Yield the p-th colon-separated directory of $PATH into `out`
 * (NUL-terminated, always trailing /).  Returns 1 on success, 0 once
 * all directories have been visited. */
static int shell_path_dir(int p, char *out, unsigned int outsz)
{
    const char *path = var_get("PATH");
    if (!path || !*path) path = "/apps";
    int idx = 0;
    while (*path) {
        const char *seg = path;
        while (*path && *path != ':') path++;
        unsigned int slen = (unsigned int)(path - seg);
        if (*path == ':') path++;
        if (slen == 0) continue;
        if (idx == p) {
            if (slen + 2 >= outsz) return 0;
            unsigned int j;
            for (j = 0; j < slen; j++) out[j] = seg[j];
            if (out[j - 1] != '/') out[j++] = '/';
            out[j] = '\0';
            return 1;
        }
        idx++;
    }
    return 0;
}

/* Returns the child's exit status (or 127 on "Unknown command", 1 on
 * fork failure) so the caller can stash it into $?.  Mirrors the
 * kernel shell's shell_dispatch_argv() ELF + makbox + PATH + nosuchcmd
 * cascade -- builtins are handled upstream in run_builtin(). */
static int run_external(int argc, char **argv)
{
    (void)argc;
    int status = 0;

    /* 1. Path-style: try /abs[.elf] or ./rel[.elf].  Never falls back
     *    to PATH or makbox -- user named a path explicitly. */
    if (looks_like_path(argv[0])) {
        if (try_exec_path(argv[0], argv, &status)) return status;
        put_s("Unknown command '"); put_s(argv[0]); put_s("' - try 'lsman'.\n");
        return 127;
    }

    /* 2. Bareword: walk $PATH, trying <dir>/<cmd>[.elf]. */
    char dir_buf[VFS_PATH_MAX];
    char path_buf[VFS_PATH_MAX];
    for (int p = 0; shell_path_dir(p, dir_buf, sizeof(dir_buf)); p++) {
        unsigned int dl = s_len(dir_buf);
        unsigned int nl = s_len(argv[0]);
        if (dl + nl + 1 >= VFS_PATH_MAX) continue;
        unsigned int i;
        for (i = 0; i < dl; i++) path_buf[i] = dir_buf[i];
        unsigned int j;
        for (j = 0; j < nl; j++) path_buf[dl + j] = argv[0][j];
        path_buf[dl + nl] = '\0';
        if (try_exec_path(path_buf, argv, &status)) return status;
    }

    /* 3. Restricted makbox fallback for known applet barewords. */
    if (is_makbox_applet(argv[0])) {
        static char makbox_argv0[] = "makbox";
        char *new_argv[MAX_ARGS + 1];
        int new_argc = 0;
        new_argv[new_argc++] = makbox_argv0;
        for (int i = 0; i < argc && new_argc < MAX_ARGS; i++)
            new_argv[new_argc++] = argv[i];
        new_argv[new_argc] = (char *)0;
        for (int p = 0; shell_path_dir(p, dir_buf, sizeof(dir_buf)); p++) {
            unsigned int dl = s_len(dir_buf);
            if (dl + sizeof(makbox_argv0) >= VFS_PATH_MAX) continue;
            unsigned int i;
            for (i = 0; i < dl; i++) path_buf[i] = dir_buf[i];
            for (i = 0; makbox_argv0[i]; i++) path_buf[dl + i] = makbox_argv0[i];
            path_buf[dl + i] = '\0';
            if (try_exec_path(path_buf, new_argv, &status)) return status;
        }
    }

    /* 4. Nothing matched: same message shape as kernel shell. */
    put_s("Unknown command '");
    put_s(argv[0]);
    put_s("' - try 'lsman'.\n");
    return 127;
}

/* ---------- main REPL ---------- */

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    put_s("sh.elf: ring-3 userspace shell (Ctrl-D or `exit` to quit)\n");

    char line[LINE_MAX];
    char expanded[LINE_MAX];        /* line post-$VAR/$? substitution */
    char dispatch[LINE_MAX];        /* tokenize() destroys its input,
                                       so we keep `line` pristine for
                                       history_push() + assignment slice. */
    char *args[MAX_ARGS];

    for (;;) {
        int n = readline("$ ", line);
        if (n == -1) {              /* EOF / Ctrl-D on empty line */
            put_c('\n');
            break;
        }
        if (n == -2) continue;      /* Ctrl-C: abort line, reprompt */

        if (n > 0) hist_push(line);

        /* Slice 20c: standalone `NAME=VAL` assignment.  RHS is shell-
         * expanded (mirrors kernel sh_script.c semantics).  We branch on
         * the raw line BEFORE expansion so `FOO=$BAR` resolves $BAR via
         * the current table, not against a half-expanded LHS. */
        const char *eq = assign_eq(line);
        if (eq) {
            char name[VAR_NAME_MAX];
            int  nlen = (int)(eq - line);
            if (nlen >= VAR_NAME_MAX) nlen = VAR_NAME_MAX - 1;
            int i;
            for (i = 0; i < nlen; i++) name[i] = line[i];
            name[i] = '\0';
            char val_expanded[VAR_VAL_MAX];
            expand(eq + 1, val_expanded, sizeof(val_expanded));
            if (var_set(name, val_expanded) != 0) {
                put_s("sh: ");
                put_s(name);
                put_s(": cannot set\n");
                g_last_status = 1;
            } else {
                g_last_status = 0;
            }
            continue;
        }

        /* Otherwise: expand variables across the whole line, then tokenize. */
        expand(line, expanded, sizeof(expanded));

        int i;
        for (i = 0; expanded[i] && i < LINE_MAX - 1; i++) dispatch[i] = expanded[i];
        dispatch[i] = '\0';

        int ac = tokenize(dispatch, args);
        if (ac == 0) continue;

        int should_exit = 0;
        int exit_status = 0;
        if (run_builtin(ac, args, &should_exit, &exit_status)) {
            if (should_exit) return exit_status;
            g_last_status = 0;
            continue;
        }
        g_last_status = run_external(ac, args);
    }

    return 0;
}
