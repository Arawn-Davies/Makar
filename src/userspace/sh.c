/*
 * sh.c -- /apps/sh.elf, the ring-3 userspace shell.
 *
 * Ports the in-kernel shell's interactive + scripting surface into
 * userspace.  Run as the per-VT login shell from boot (the
 * user_shell_slot_entry path in kernel/kernel.c), or as a nested shell
 * via `exec /apps/sh.elf`.
 *
 * Argv:
 *   sh.elf            Nested mode -- exits cleanly on `exit` or Ctrl-D.
 *   sh.elf --login    VT-owning login shell -- reprompts on `exit`/Ctrl-D.
 *   sh.elf <script>   Run <script> and exit (positional, not yet wired
 *                     into the userspace boot path but used by `./foo.sh`).
 *
 * Features (parity with the in-kernel shell):
 *   - Readline with cursor edit + 16-entry history.
 *   - Zsh-style tab cycling on the current path component.
 *   - Glob expansion of `*` and `?` against the cwd.
 *   - $VAR / ${VAR} / $? expansion across the whole line.
 *   - PATH lookup with default `/apps`, `.elf` auto-probe.
 *   - Restricted makbox fallback for ls/cat/cp/mv/rm/mkdir/rmdir/echo/pwd.
 *   - Builtins: cd, pwd, exit, env, unset, read, sleep, true, false,
 *               `[ ... ]`, history, hostname.
 *   - Admin builtins via privileged syscalls: shutdown, reboot, eject,
 *               setmode, fgcol, bgcol, mount, umount, mkfs.ext2,
 *               mkfs.fat32, sched_quantum, verbose.
 *   - Stub builtins (rejected with a stable message): install, chainload,
 *               readsector, mkpart, lspart, ktest.
 *   - Scripting: `;`-separated statements, `#` comments, if/elif/else/fi,
 *               while, for ... in, sh <file>, ./script.sh
 *
 * Freestanding: only depends on <syscall.h>.  TCC must be able to rebuild
 * this in-OS (no libc, no GCC builtins, no float).
 */

#include "syscall.h"

#define LINE_MAX        512
#define SCRIPT_LINE_MAX 1024     /* loops/conditionals can accumulate */
#define MAX_ARGS        64       /* generous: glob may explode tokens */
#define HIST_MAX        16
#define VAR_MAX         32
#define VAR_NAME_MAX    32
#define VAR_VAL_MAX     192
#define VFS_PATH_MAX    256
#define HOST_MAX        64
#define USER_MAX        32

/* ---------- mode flags ---------- */
static int   g_login        = 0;  /* --login: don't exit on Ctrl-D / `exit` */
static int   g_autostart_gui = 0; /* --autostart=gui: run `gui` after rc      */
static int   g_quiet_start  = 0;
static char  g_hostname[HOST_MAX] = "makar";
static char  g_username[USER_MAX] = "root";

/* ---------- string helpers ---------- */

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
static int s_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}
static int s_has_char(const char *s, char needle)
{
    while (*s) { if (*s++ == needle) return 1; }
    return 0;
}
static void s_copy(char *dst, const char *src, unsigned int cap)
{
    unsigned int i;
    if (cap == 0) return;
    for (i = 0; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}
static void put_s(const char *s) { sys_write(1, s, s_len(s)); }
static void put_c(char c)        { sys_write(1, &c, 1); }
static int  s_atoi(const char *s)
{
    int sign = 1;
    int v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

static void ignore_login_shell_signals(void)
{
    sys_signal(SIGHUP,  SIG_IGN);
    sys_signal(SIGINT,  SIG_IGN);
    sys_signal(SIGQUIT, SIG_IGN);
    sys_signal(SIGPIPE, SIG_IGN);
    sys_signal(SIGALRM, SIG_IGN);
    sys_signal(SIGTERM, SIG_IGN);
    sys_signal(SIGTSTP, SIG_IGN);
}

/* ---------- variable table ---------- */

typedef struct { char name[VAR_NAME_MAX]; char val[VAR_VAL_MAX]; } var_t;
static var_t        g_vars[VAR_MAX];
static unsigned int g_var_count   = 0;
static int          g_last_status = 0;

static int var_name_ok(const char *n)
{
    if (!*n) return 0;
    if (n[0] >= '0' && n[0] <= '9') return 0;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0;
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
    for (unsigned int i = 0; i < g_var_count; i++) {
        if (s_eq(g_vars[i].name, name)) {
            s_copy(g_vars[i].val, val, VAR_VAL_MAX);
            return 0;
        }
    }
    if (g_var_count >= VAR_MAX) return -1;
    s_copy(g_vars[g_var_count].name, name, VAR_NAME_MAX);
    s_copy(g_vars[g_var_count].val,  val,  VAR_VAL_MAX);
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

static unsigned int sleep_ticks_from_arg(const char *s)
{
    unsigned int whole = 0;
    unsigned int frac = 0;
    unsigned int scale = 1;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10u + (unsigned int)(*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale < 100u) {
            frac = frac * 10u + (unsigned int)(*s - '0');
            scale *= 10u;
            s++;
        }
    }
    return whole * 100u + (frac * 100u + scale - 1u) / scale;
}

/* Expand $VAR / ${VAR} / $? in `in` -> `out`. */
static void expand(const char *in, char *out, unsigned int outsz)
{
    unsigned int o = 0;
    while (*in && o + 1 < outsz) {
        if (*in != '$') { out[o++] = *in++; continue; }
        in++;
        if (*in == '?') {
            char num[12];
            status_str(num, g_last_status);
            for (unsigned int j = 0; num[j] && o + 1 < outsz; j++) out[o++] = num[j];
            in++; continue;
        }
        char name[VAR_NAME_MAX];
        unsigned int n = 0;
        int braced = 0;
        if (*in == '{') { braced = 1; in++; }
        while (*in && n + 1 < VAR_NAME_MAX) {
            char c = *in;
            int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_';
            if (!ok) break;
            name[n++] = c; in++;
        }
        name[n] = '\0';
        if (braced && *in == '}') in++;
        if (n == 0) { if (o + 1 < outsz) out[o++] = '$'; continue; }
        const char *v = var_get(name);
        if (v) for (unsigned int j = 0; v[j] && o + 1 < outsz; j++) out[o++] = v[j];
    }
    out[o] = '\0';
}

/* Detect a standalone `NAME=...` assignment.  Returns the '=' pointer. */
static const char *assign_eq(const char *line)
{
    if (!line || !*line) return (const char *)0;
    const char *p = line;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
        return (const char *)0;
    p++;
    while (*p && *p != '=' && *p != ' ' && *p != '\t') {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return (const char *)0;
        p++;
    }
    return *p == '=' ? p : (const char *)0;
}

/* ---------- history ---------- */

static char         g_hist[HIST_MAX][LINE_MAX];
static unsigned int g_hist_count = 0;
static unsigned int g_hist_head  = 0;

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
    const char *latest = hist_get(0);
    if (latest && s_eq(latest, s)) return;
    if (n >= LINE_MAX) n = LINE_MAX - 1;
    char *slot = g_hist[g_hist_head];
    for (unsigned int i = 0; i < n; i++) slot[i] = s[i];
    slot[n] = '\0';
    g_hist_head = (g_hist_head + 1) % HIST_MAX;
    if (g_hist_count < HIST_MAX) g_hist_count++;
}

/* ---------- tab completion (zsh cycle) ---------- */

/* Identify the start of the rightmost path component in `buf`. */
static unsigned int rightmost_token_start(const char *buf, unsigned int len)
{
    unsigned int s = len;
    while (s > 0) {
        char c = buf[s - 1];
        if (c == ' ' || c == '\t') break;
        s--;
    }
    return s;
}

/* Split <prefix>/<name-prefix> from a path-like token.  Out: dir gets
 * everything up to and including the last '/' (or empty for bare names),
 * leaf gets what's after.  cwd is used when token has no '/'. */
static void split_path_token(const char *token, char *dir, unsigned int dirsz,
                             const char *cwd, char *leaf, unsigned int leafsz)
{
    const char *slash = (const char *)0;
    for (const char *p = token; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        unsigned int dl = (unsigned int)(slash - token) + 1;  /* include slash */
        if (dl >= dirsz) dl = dirsz - 1;
        unsigned int i;
        for (i = 0; i < dl; i++) dir[i] = token[i];
        dir[i] = '\0';
        s_copy(leaf, slash + 1, leafsz);
    } else {
        s_copy(dir, cwd, dirsz);
        s_copy(leaf, token, leafsz);
    }
}

/* Resolve a relative `dir` against cwd.  Output absolute path. */
static void resolve_abs(const char *dir, char *out, unsigned int outsz)
{
    if (!dir || !*dir) { s_copy(out, "/", outsz); return; }
    if (dir[0] == '/') { s_copy(out, dir, outsz); return; }
    char cwd[VFS_PATH_MAX];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) cwd[0] = '/', cwd[1] = '\0';
    unsigned int cl = s_len(cwd);
    unsigned int o = 0;
    for (unsigned int i = 0; i < cl && o + 1 < outsz; i++) out[o++] = cwd[i];
    if (o > 0 && out[o - 1] != '/' && o + 1 < outsz) out[o++] = '/';
    for (unsigned int i = 0; dir[i] && o + 1 < outsz; i++) out[o++] = dir[i];
    out[o] = '\0';
}

#define TAB_MAX  32

typedef struct {
    char name[64];
    int  is_dir;
} tab_match_t;

static int shell_path_dir(int p, char *out, unsigned int outsz);

static int tab_match_add(tab_match_t *out, int n, const char *name, int is_dir)
{
    if (!name || !*name || n >= TAB_MAX) return n;
    for (int i = 0; i < n; i++)
        if (s_eq(out[i].name, name)) return n;
    s_copy(out[n].name, name, sizeof(out[n].name));
    out[n].is_dir = is_dir;
    return n + 1;
}

/* Enumerate matches for `leaf` inside `abs_dir` via sys_readdir.  Returns
 * count (capped at TAB_MAX). */
static int tab_collect(const char *abs_dir, const char *leaf,
                       tab_match_t *out)
{
    int n = 0;
    unsigned int leaf_len = s_len(leaf);
    struct dirent de;
    for (unsigned int idx = 0; n < TAB_MAX; idx++) {
        int rc = sys_readdir(abs_dir, idx, &de);
        if (rc <= 0) break;
        if (de.d_name[0] == '\0') continue;
        if (de.d_name[0] == '.' && leaf[0] != '.') continue;  /* hide dotfiles */
        if (leaf_len > 0) {
            int match = 1;
            for (unsigned int j = 0; j < leaf_len; j++)
                if (de.d_name[j] != leaf[j]) { match = 0; break; }
            if (!match) continue;
        }
        n = tab_match_add(out, n, de.d_name, de.d_type == DT_DIR);
    }
    return n;
}

static int tab_collect_commands(const char *leaf, tab_match_t *out)
{
    int n = 0;
    unsigned int leaf_len = s_len(leaf);
    static const char *builtins[] = {
        "exit", "logout", "cd", "pwd", "env", "unset", "read", "true", "false",
        "sleep", "[", "history", "hostname", "clear", "exec", "sh", ".",
        "shutdown", "reboot", "eject", "setmode", "fgcol", "bgcol",
        "mount", "umount", "mkfs.ext2", "mkfs.fat32", "sched_quantum",
        "verbose", "install", "chainload", "readsector", "mkpart",
        "lspart", "ktest", (const char *)0
    };
    static const char *applets[] = {
        "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "echo", "pwd", (const char *)0
    };

    for (int i = 0; builtins[i] && n < TAB_MAX; i++) {
        if (leaf_len > 0 && !s_starts(builtins[i], leaf)) continue;
        n = tab_match_add(out, n, builtins[i], 0);
    }
    for (int i = 0; applets[i] && n < TAB_MAX; i++) {
        if (leaf_len > 0 && !s_starts(applets[i], leaf)) continue;
        n = tab_match_add(out, n, applets[i], 0);
    }

    char dir[VFS_PATH_MAX];
    for (int p = 0; shell_path_dir(p, dir, sizeof(dir)) && n < TAB_MAX; p++) {
        struct dirent de;
        for (unsigned int idx = 0; n < TAB_MAX; idx++) {
            int rc = sys_readdir(dir, idx, &de);
            if (rc <= 0) break;
            if (de.d_name[0] == '\0' || de.d_type == DT_DIR) continue;
            char name[64];
            s_copy(name, de.d_name, sizeof(name));
            unsigned int nl = s_len(name);
            if (nl > 4 && name[nl - 4] == '.' && name[nl - 3] == 'e' &&
                name[nl - 2] == 'l' && name[nl - 1] == 'f')
                name[nl - 4] = '\0';
            if (leaf_len > 0 && !s_starts(name, leaf)) continue;
            n = tab_match_add(out, n, name, 0);
        }
    }
    return n;
}

/* Longest common prefix of n matches (writes NUL-terminated `out`). */
static unsigned int tab_lcp(const tab_match_t *m, int n, char *out, unsigned int outsz)
{
    if (n <= 0) { if (outsz) out[0] = '\0'; return 0; }
    unsigned int p = 0;
    for (;; p++) {
        char c = m[0].name[p];
        if (c == '\0') break;
        for (int i = 1; i < n; i++)
            if (m[i].name[p] != c) goto done;
    }
done:
    if (p >= outsz) p = outsz - 1;
    for (unsigned int i = 0; i < p; i++) out[i] = m[0].name[i];
    out[p] = '\0';
    return p;
}

/* ---------- readline with tab cycle ---------- */

/* readline result: 0+ = length, -1 = EOF/Ctrl-D, -2 = Ctrl-C aborted. */
static int readline(const char *prompt, char *buf)
{
    put_s(prompt);

    unsigned int len = 0;
    unsigned int cur = 0;
    int          hist_idx = -1;
    char         saved[LINE_MAX];
    saved[0] = '\0';
    unsigned int saved_len = 0;

    /* Tab-cycle state: when active, repeated Tabs cycle tc_matches. */
    int          tc_active = 0;
    tab_match_t  tc_m[TAB_MAX];
    int          tc_n = 0;
    int          tc_idx = 0;
    unsigned int tc_token_start = 0;   /* offset of completing component in buf */
    unsigned int tc_committed_len = 0; /* length committed pre-cycle */

    for (;;) {
        int c = sys_getkey();
        if (c < 0) continue;
        unsigned char ch = (unsigned char)c;

        /* Any non-Tab key commits the in-progress cycle. */
        if (tc_active && ch != '\t') tc_active = 0;

        if (ch == '\n' || ch == '\r') { put_c('\n'); buf[len] = '\0'; return (int)len; }
        if (ch == 0x03) { put_s("^C\n"); buf[0] = '\0'; return -2; }
        if (ch == 0x04) { if (len == 0) return -1; continue; }
        if (ch == 0x08 || ch == 0x7F) {
            if (cur == 0) continue;
            for (unsigned int i = cur - 1; i + 1 < len; i++) buf[i] = buf[i + 1];
            len--; cur--;
            put_c('\b');
            if (len > cur) sys_write(1, &buf[cur], len - cur);
            put_c(' ');
            for (unsigned int i = 0; i <= len - cur; i++) put_c('\b');
            continue;
        }
        if (ch == KEY_ARROW_LEFT)  { if (cur > 0)   { put_c('\b'); cur--; } continue; }
        if (ch == KEY_ARROW_RIGHT) { if (cur < len) { put_c(buf[cur]); cur++; } continue; }
        if (ch == KEY_ARROW_UP || ch == KEY_ARROW_DOWN) {
            int new_idx;
            if (ch == KEY_ARROW_UP) {
                new_idx = (hist_idx < 0) ? 0 : hist_idx + 1;
                if ((unsigned int)new_idx >= g_hist_count) continue;
                if (hist_idx < 0) {
                    for (unsigned int i = 0; i < len; i++) saved[i] = buf[i];
                    saved[len] = '\0';
                    saved_len = len;
                }
            } else {
                if (hist_idx < 0) continue;
                new_idx = hist_idx - 1;
            }
            for (unsigned int i = 0; i < cur; i++) put_c('\b');
            for (unsigned int i = 0; i < len; i++) put_c(' ');
            for (unsigned int i = 0; i < len; i++) put_c('\b');
            const char *src; unsigned int n;
            if (new_idx < 0) { src = saved; n = saved_len; }
            else { src = hist_get((unsigned int)new_idx); n = src ? s_len(src) : 0; }
            if (n >= LINE_MAX) n = LINE_MAX - 1;
            for (unsigned int i = 0; i < n; i++) buf[i] = src[i];
            buf[n] = '\0';
            len = n; cur = n;
            if (len > 0) sys_write(1, buf, len);
            hist_idx = new_idx;
            continue;
        }

        /* ---- Tab completion ---- */
        if (ch == '\t') {
            /* Only complete at end-of-line (matches kernel shell). */
            if (cur != len) continue;

            if (tc_active && tc_n > 1) {
                /* Cycle: erase current candidate, draw next. */
                tc_idx = (tc_idx + 1) % tc_n;
                while (len > tc_committed_len) { put_c('\b'); put_c(' '); put_c('\b'); len--; cur--; }
                const char *m = tc_m[tc_idx].name;
                unsigned int ml = s_len(m);
                /* Skip the part already in buf (committed_len - token_start) */
                unsigned int already = tc_committed_len - tc_token_start;
                for (unsigned int i = already; i < ml && len < LINE_MAX - 1; i++) {
                    buf[len++] = m[i]; put_c(m[i]); cur = len;
                }
                if (tc_m[tc_idx].is_dir && len < LINE_MAX - 1) {
                    buf[len++] = '/'; put_c('/'); cur = len;
                } else if (!tc_m[tc_idx].is_dir && len < LINE_MAX - 1) {
                    /* No trailing space on cycle so user can keep tabbing */
                }
                continue;
            }

            /* First Tab: collect matches. */
            unsigned int ts = rightmost_token_start(buf, len);
            char token[VFS_PATH_MAX];
            unsigned int tlen = len - ts;
            if (tlen >= sizeof(token)) tlen = sizeof(token) - 1;
            for (unsigned int i = 0; i < tlen; i++) token[i] = buf[ts + i];
            token[tlen] = '\0';

            char cwd[VFS_PATH_MAX];
            if (sys_getcwd(cwd, sizeof(cwd)) < 0) { cwd[0] = '/'; cwd[1] = '\0'; }

            char dir[VFS_PATH_MAX], leaf[VFS_PATH_MAX], abs_dir[VFS_PATH_MAX];
            split_path_token(token, dir, sizeof(dir), cwd, leaf, sizeof(leaf));
            resolve_abs(dir, abs_dir, sizeof(abs_dir));

            if (ts == 0 && token[0] != '/' && !s_has_char(token, '/'))
                tc_n = tab_collect_commands(leaf, tc_m);
            else
                tc_n = tab_collect(abs_dir, leaf, tc_m);
            if (tc_n == 0) continue;

            char lcp[VFS_PATH_MAX];
            unsigned int lcp_len = tab_lcp(tc_m, tc_n, lcp, sizeof(lcp));

            /* Extend buf to LCP. */
            unsigned int leaf_len = s_len(leaf);
            if (lcp_len > leaf_len) {
                for (unsigned int i = leaf_len; i < lcp_len && len < LINE_MAX - 1; i++) {
                    buf[len++] = lcp[i]; put_c(lcp[i]); cur = len;
                }
            }

            if (tc_n == 1) {
                if (tc_m[0].is_dir && len < LINE_MAX - 1) { buf[len++] = '/'; put_c('/'); cur = len; }
                else if (len < LINE_MAX - 1)              { buf[len++] = ' '; put_c(' '); cur = len; }
                tc_active = 0;
            } else if (lcp_len == leaf_len) {
                /* Already at LCP -- start cycling. */
                tc_active = 1; tc_idx = 0;
                tc_token_start = ts;
                tc_committed_len = len;
                /* Show the first candidate immediately. */
                const char *m0 = tc_m[0].name;
                unsigned int ml = s_len(m0);
                unsigned int already = tc_committed_len - tc_token_start;
                for (unsigned int i = already; i < ml && len < LINE_MAX - 1; i++) {
                    buf[len++] = m0[i]; put_c(m0[i]); cur = len;
                }
                if (tc_m[0].is_dir && len < LINE_MAX - 1) {
                    buf[len++] = '/'; put_c('/'); cur = len;
                }
            } else {
                /* Extended to LCP; user may Tab again to enter cycle. */
                tc_active = 0;
            }
            continue;
        }

        if (ch < 0x20 || ch >= 0x80) continue;
        if (len >= LINE_MAX - 1) continue;
        for (unsigned int i = len; i > cur; i--) buf[i] = buf[i - 1];
        buf[cur] = (char)ch; len++; cur++;
        sys_write(1, &buf[cur - 1], len - (cur - 1));
        for (unsigned int i = 0; i < len - cur; i++) put_c('\b');
    }
}

/* ---------- tokenizer + glob ---------- */

/* split `line` in-place on whitespace, honouring quotes.  argv[0..argc-1]
 * = tokens.  '...' and "..." group runs (whitespace inside a quote does
 * not split) and the quote characters themselves are removed.  Variable
 * expansion already happened in run_line, so quotes here are purely about
 * word-splitting + literal removal.  Compaction is in-place: the write
 * pointer never overtakes the read pointer because we only ever drop the
 * two quote bytes per quoted run. */
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        char *w = p;                 /* compacted write cursor */
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {        /* single quote: copy literally */
                p++;
                while (*p && *p != '\'') *w++ = *p++;
                if (*p == '\'') p++;
            } else if (*p == '"') {  /* double quote: same (already expanded) */
                p++;
                while (*p && *p != '"') *w++ = *p++;
                if (*p == '"') p++;
            } else {
                *w++ = *p++;
            }
        }
        char term = *p;              /* delimiter (space/tab) or NUL */
        *w = '\0';                   /* terminate compacted token (w <= p) */
        if (term) p++;               /* step over the delimiter */
    }
    argv[argc] = (char *)0;
    return argc;
}

static int has_glob_char(const char *s)
{
    for (const char *p = s; *p; p++) if (*p == '*' || *p == '?') return 1;
    return 0;
}

/* fnmatch-lite: ? matches one, * matches any.  No bracket sets. */
static int glob_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*s) { if (glob_match(pat, s)) return 1; s++; }
            return 0;
        }
        if (!*s) return 0;
        if (*pat != '?' && *pat != *s) return 0;
        pat++; s++;
    }
    return !*s;
}

/* Expand any tokens containing globs against the cwd.  Returns new argc.
 * Storage backs all expanded names; caller provides a scratch buffer. */
static int expand_globs(int argc, char **argv, int cap,
                        char *storage, unsigned int storage_size)
{
    char *sp = storage;
    char *sp_end = storage + storage_size;
    int out_argc = 0;
    char *new_argv[MAX_ARGS];

    char cwd[VFS_PATH_MAX];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) { cwd[0] = '/'; cwd[1] = '\0'; }

    for (int i = 0; i < argc; i++) {
        if (!has_glob_char(argv[i])) {
            if (out_argc < cap - 1) new_argv[out_argc++] = argv[i];
            continue;
        }
        /* Pattern is glob against cwd (no slashes supported in this slice). */
        struct dirent de;
        int matched_any = 0;
        for (unsigned int idx = 0; out_argc < cap - 1; idx++) {
            int rc = sys_readdir(cwd, idx, &de);
            if (rc <= 0) break;
            if (de.d_name[0] == '\0') continue;
            if (de.d_name[0] == '.' && argv[i][0] != '.') continue;
            if (!glob_match(argv[i], de.d_name)) continue;
            unsigned int nl = s_len(de.d_name);
            if (sp + nl + 1 >= sp_end) break;
            for (unsigned int j = 0; j <= nl; j++) sp[j] = de.d_name[j];
            new_argv[out_argc++] = sp;
            sp += nl + 1;
            matched_any = 1;
        }
        if (!matched_any && out_argc < cap - 1) {
            /* No matches: keep literal (POSIX sh behaviour). */
            new_argv[out_argc++] = argv[i];
        }
    }
    new_argv[out_argc] = (char *)0;
    for (int i = 0; i <= out_argc; i++) argv[i] = new_argv[i];
    return out_argc;
}

/* ---------- `[` test evaluator ---------- */

/* Strip trailing `]` from argv before evaluating.  Returns 0 = true,
 * 1 = false, 2 = syntax error (matches POSIX `[`). */
static int test_eval(int argc, char **argv)
{
    /* argv[0] = "[", last must be "]". */
    if (argc < 2 || !s_eq(argv[argc - 1], "]")) {
        put_s("[: missing closing ']'\n"); return 2;
    }
    int n = argc - 2;  /* drop "[" + "]" */
    char **a = argv + 1;
    if (n == 0) return 1;
    if (n == 1) {
        return (a[0][0] == '\0') ? 1 : 0;
    }
    if (n == 2 && s_eq(a[0], "-z")) return (a[1][0] == '\0') ? 0 : 1;
    if (n == 2 && s_eq(a[0], "-n")) return (a[1][0] != '\0') ? 0 : 1;
    if (n == 2 && s_eq(a[0], "!"))  return (a[1][0] == '\0') ? 0 : 1;
    if (n == 3) {
        const char *op = a[1];
        if (s_eq(op, "=") || s_eq(op, "=="))  return s_eq(a[0], a[2]) ? 0 : 1;
        if (s_eq(op, "!="))                   return s_eq(a[0], a[2]) ? 1 : 0;
        /* Integer comparisons. */
        int aok = 1, bok = 1;
        for (const char *p = a[0]; *p; p++)
            if (!((p == a[0] && *p == '-') || (*p >= '0' && *p <= '9'))) { aok = 0; break; }
        for (const char *p = a[2]; *p; p++)
            if (!((p == a[2] && *p == '-') || (*p >= '0' && *p <= '9'))) { bok = 0; break; }
        if (!aok || !bok) { put_s("[: integer expected\n"); return 2; }
        int la = s_atoi(a[0]), lb = s_atoi(a[2]);
        if (s_eq(op, "-eq")) return (la == lb) ? 0 : 1;
        if (s_eq(op, "-ne")) return (la != lb) ? 0 : 1;
        if (s_eq(op, "-lt")) return (la <  lb) ? 0 : 1;
        if (s_eq(op, "-le")) return (la <= lb) ? 0 : 1;
        if (s_eq(op, "-gt")) return (la >  lb) ? 0 : 1;
        if (s_eq(op, "-ge")) return (la >= lb) ? 0 : 1;
    }
    put_s("[: bad expression\n");
    return 2;
}

/* ---------- forward decls ---------- */

static int run_line(const char *raw, int *should_exit, int *exit_status);
static int run_script_buf(const char *src);
static int try_exec_path(const char *path, char **argv, int *out_status);
static int jobs_wait_all(void);

/* ---------- admin command bareword routing ---------- */

/* Returns 1 if argv[0] was an admin command (and was handled). */
static int run_admin(int argc, char **argv)
{
    const char *cmd = argv[0];
    if (s_eq(cmd, "shutdown"))     { sys_shutdown(); return 1; }
    if (s_eq(cmd, "reboot"))       { sys_reboot();   return 1; }
    if (s_eq(cmd, "eject"))        { g_last_status = sys_eject() ? 1 : 0; return 1; }
    if (s_eq(cmd, "install"))      { g_last_status = sys_install() ? 1 : 0; return 1; }
    if (s_eq(cmd, "setmode")) {
        int rc = sys_setmode(argc >= 2 ? argv[1] : (const char *)0);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "fgcol")) {
        int rc = sys_fgcol(argc >= 2 ? argv[1] : (const char *)0);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "bgcol")) {
        int rc = sys_bgcol(argc >= 2 ? argv[1] : (const char *)0);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "mount")) {
        int rc = sys_mount(argc >= 2 ? argv[1] : (const char *)0,
                           argc >= 3 ? argv[2] : (const char *)0);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "umount")) {
        int rc = sys_umount(argc >= 2 ? argv[1] : (const char *)0);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "mkfs.ext2")) {
        if (argc < 2) { put_s("Usage: mkfs.ext2 /dev/hdaN\n"); g_last_status = 1; return 1; }
        int rc = sys_mkfs(argv[1], "ext2"); g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "mkfs.fat32")) {
        if (argc < 2) { put_s("Usage: mkfs.fat32 /dev/hdaN\n"); g_last_status = 1; return 1; }
        int rc = sys_mkfs(argv[1], "fat32"); g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "sched_quantum")) {
        int new_v = argc >= 2 ? s_atoi(argv[1]) : -1;
        int rc = sys_sched_quantum(new_v);
        g_last_status = rc < 0 ? 1 : 0; return 1;
    }
    if (s_eq(cmd, "verbose")) {
        int new_v;
        if (argc < 2) new_v = -1;
        else if (s_eq(argv[1], "on"))  new_v = 1;
        else if (s_eq(argv[1], "off")) new_v = 0;
        else { put_s("Usage: verbose [on|off]\n"); g_last_status = 1; return 1; }
        sys_verbose(new_v); g_last_status = 0; return 1;
    }
    /* Visible stubs -- not yet wrappable from userspace.  Stable text. */
    if (s_eq(cmd, "chainload")  ||
        s_eq(cmd, "readsector") || s_eq(cmd, "mkpart")    ||
        s_eq(cmd, "lspart")     || s_eq(cmd, "ktest")) {
        put_s(cmd); put_s(": not available from userspace yet\n");
        g_last_status = 1;
        return 1;
    }
    return 0;
}

/* ---------- builtins ---------- */

/* Returns 1 if handled.  Sets *should_exit when the shell terminates. */
static int run_builtin(int argc, char **argv, int *should_exit, int *exit_status)
{
    if (argc == 0) return 1;

    if (s_eq(argv[0], "exit")) {
        *exit_status = (argc > 1) ? s_atoi(argv[1]) : 0;
        if (g_login) {
            put_s("sh: cannot exit a login shell -- use `logout`, `shutdown`, or `reboot`\n");
            return 1;
        }
        *should_exit = 1;
        return 1;
    }
    if (s_eq(argv[0], "logout")) {
        if (!g_login) {
            put_s("sh: not a login shell\n");
            g_last_status = 1;
            return 1;
        }
        *exit_status = 0;
        *should_exit = 1;
        return 1;
    }
    if (s_eq(argv[0], "cd")) {
        const char *target = (argc > 1) ? argv[1] : "/";
        if (sys_chdir(target) != 0) {
            put_s("cd: "); put_s(target); put_s(": no such directory\n");
            g_last_status = 1;
        } else g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "pwd")) {
        char buf[VFS_PATH_MAX];
        int n = sys_getcwd(buf, sizeof(buf));
        if (n < 0) { put_s("pwd: error\n"); g_last_status = 1; }
        else {
            /* Emit the same serial-only marker that /apps/makbox.elf's
             * pwd applet prints (`[makbox:pwd] `), so the in-guest test drivers
             * that assert on it via `assert_serial_contains` keep working
             * regardless of whether `pwd` is dispatched as a sh.elf
             * builtin or routed through makbox.  Screen output stays
             * unchanged (just the cwd path). */
            sys_write_serial("[makbox:pwd] ", 13);
            put_s(buf); put_c('\n');
            g_last_status = 0;
        }
        return 1;
    }
    if (s_eq(argv[0], "env")) {
        for (unsigned int i = 0; i < g_var_count; i++) {
            put_s(g_vars[i].name); put_c('='); put_s(g_vars[i].val); put_c('\n');
        }
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "unset")) {
        for (int i = 1; i < argc; i++) var_unset(argv[i]);
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "read")) {
        if (argc < 2) { put_s("read: missing variable name\n"); g_last_status = 1; return 1; }
        if (!var_name_ok(argv[1])) { put_s("read: invalid name\n"); g_last_status = 1; return 1; }
        char buf[LINE_MAX];
        int n = readline("", buf);
        if (n < 0) { var_set(argv[1], ""); g_last_status = 1; return 1; }
        var_set(argv[1], buf);
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "true"))  { g_last_status = 0; return 1; }
    if (s_eq(argv[0], "false")) { g_last_status = 1; return 1; }
    if (s_eq(argv[0], "wait")) {
        /* `wait` with no args drains the background-jobs table.
         * `wait <pid>` waits on a specific pid (not in our table; still
         * passes through sys_wait4 in case the caller knows the pid). */
        if (argc == 1) {
            g_last_status = jobs_wait_all();
        } else {
            int pid = s_atoi(argv[1]);
            int st  = 0;
            int r   = sys_wait4(pid, &st, 0);
            g_last_status = (r < 0) ? 127 : (st & 0xFF);
        }
        return 1;
    }
    if (s_eq(argv[0], "sleep")) {
        if (argc < 2) { put_s("Usage: sleep <seconds>\n"); g_last_status = 1; return 1; }
        unsigned int ticks = sleep_ticks_from_arg(argv[1]);
        unsigned int start = sys_uptime();
        unsigned int target = start + ticks;
        while (sys_uptime() < target) sys_yield();
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "[")) {
        g_last_status = test_eval(argc, argv);
        return 1;
    }
    if (s_eq(argv[0], "history")) {
        for (unsigned int i = g_hist_count; i > 0; i--) {
            const char *h = hist_get(i - 1);
            char num[12]; status_str(num, (int)(g_hist_count - i + 1));
            put_s(num); put_c(' '); put_s(h); put_c('\n');
        }
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "hostname")) {
        put_s(g_hostname); put_c('\n');
        g_last_status = 0;
        return 1;
    }
    if (s_eq(argv[0], "clear")) {
        sys_shell_clear(); g_last_status = 0; return 1;
    }
    if (s_eq(argv[0], "exec")) {
        /* exec PATH [args...] -- run ELF and wait, matching the
         * kernel shell's `exec` semantics (NOT POSIX exec, which
         * would replace the shell process).  Keeps existing UI test
         * scenarios and operator muscle memory working.  See
         * docs/plans/posix-shell.md for the POSIX-exec follow-up. */
        if (argc < 2) { put_s("exec: missing path\n"); g_last_status = 1; return 1; }
        char *child_argv[MAX_ARGS];
        int   cac = 0;
        for (int i = 1; i < argc && cac < MAX_ARGS - 1; i++) child_argv[cac++] = argv[i];
        child_argv[cac] = (char *)0;
        int status = 0;
        if (try_exec_path(child_argv[0], child_argv, &status)) {
            g_last_status = status;
        } else {
            put_s("exec: "); put_s(argv[1]); put_s(": not found\n");
            g_last_status = 127;
        }
        return 1;
    }
    if (s_eq(argv[0], "sh") || s_eq(argv[0], ".")) {
        if (argc < 2) { put_s("sh: missing script path\n"); g_last_status = 1; return 1; }
        char *buf = (char *)0;
        int fd = sys_open(argv[1], O_RDONLY);
        if (fd < 0) { put_s("sh: cannot open "); put_s(argv[1]); put_c('\n'); g_last_status = 1; return 1; }
        struct stat st;
        if (sys_fstat(fd, &st) < 0 || st.st_size == 0) { sys_close(fd); g_last_status = 1; return 1; }
        /* Stack-buffer for small scripts (most are <16k). */
        static char script_buf[16384];
        unsigned int cap = sizeof(script_buf) - 1;
        unsigned int n = st.st_size > cap ? cap : st.st_size;
        int rd = sys_read(fd, script_buf, n);
        sys_close(fd);
        if (rd <= 0) { g_last_status = 1; return 1; }
        script_buf[rd] = '\0';
        buf = script_buf;
        g_last_status = run_script_buf(buf);
        return 1;
    }
    return 0;
}

/* ---------- external command dispatch (PATH + makbox) ---------- */

static int is_makbox_applet(const char *name)
{
    static const char *applets[] = {
        "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "echo", "pwd", (const char *)0
    };
    for (int i = 0; applets[i]; i++) if (s_eq(name, applets[i])) return 1;
    return 0;
}
static int looks_like_path(const char *s)
{
    return s[0] == '/' || (s[0] == '.' && s[1] == '/');
}
static int path_exists(const char *path)
{
    struct stat st; return sys_stat(path, &st) == 0;
}
/* Per-command redirect bookkeeping is defined further down (after the
 * redirect_t typedef in the pipeline support block).  We forward-declare
 * just the helper that spawn()'s child uses. */
static void apply_pending_redirects_in_child(void);
static void jobs_add(int pid);

static int spawn(const char *path, char **argv)
{
    int pid = sys_fork();
    if (pid < 0) { put_s("sh: fork failed\n"); return 1; }
    if (pid == 0) {
        apply_pending_redirects_in_child();
        sys_execve(path, argv, (char *const *)0);
        sys_exit(127);
    }
    int status = 0;
    sys_wait4(pid, &status, 0);
    return status & 0xFF;
}

static int spawn_background(const char *path, char **argv)
{
    int pid = sys_fork();
    if (pid < 0) { put_s("sh: fork failed\n"); return 1; }
    if (pid == 0) {
        apply_pending_redirects_in_child();
        sys_execve(path, argv, (char *const *)0);
        sys_exit(127);
    }
    jobs_add(pid);
    return 0;
}

static int is_gui_path(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return s_eq(base, "gui") || s_eq(base, "gui.elf");
}

static int try_exec_path(const char *path, char **argv, int *out_status)
{
    if (path_exists(path)) {
        *out_status = is_gui_path(path) ? spawn_background(path, argv)
                                        : spawn(path, argv);
        return 1;
    }
    char wext[VFS_PATH_MAX];
    unsigned int plen = s_len(path);
    if (plen + 5 >= VFS_PATH_MAX) return 0;
    unsigned int i;
    for (i = 0; i < plen; i++) wext[i] = path[i];
    wext[i++] = '.'; wext[i++] = 'e'; wext[i++] = 'l'; wext[i++] = 'f'; wext[i] = '\0';
    if (path_exists(wext)) {
        *out_status = is_gui_path(wext) ? spawn_background(wext, argv)
                                        : spawn(wext, argv);
        return 1;
    }
    return 0;
}
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
/* Fullscreen apps that, while makmux is running, open in their own named tab
 * instead of taking over the current VT (matches the kernel shell's notion of
 * "fullscreen" commands). */
static int is_tab_app(const char *name)
{
    static const char *apps[] = { "vix", "maktop", "clock", "cfdisk",
                                  "basic", "kbtester", 0 };
    for (int i = 0; apps[i]; i++)
        if (s_eq(name, apps[i])) return 1;
    return 0;
}

/* Resolve a bare command name to a full path via PATH (with .elf fallback). */
static int resolve_app_path(const char *name, char *out, unsigned int sz)
{
    char dir[VFS_PATH_MAX];
    for (int p = 0; shell_path_dir(p, dir, sizeof(dir)); p++) {
        unsigned int dl = s_len(dir), nl = s_len(name);
        if (dl + nl + 5 >= sz) continue;
        unsigned int i;
        for (i = 0; i < dl; i++) out[i] = dir[i];
        for (unsigned int j = 0; j < nl; j++) out[dl + j] = name[j];
        out[dl + nl] = '\0';
        if (path_exists(out)) return 1;
        out[dl+nl]='.'; out[dl+nl+1]='e'; out[dl+nl+2]='l';
        out[dl+nl+3]='f'; out[dl+nl+4]='\0';
        if (path_exists(out)) return 1;
    }
    return 0;
}

static int run_external(int argc, char **argv)
{
    (void)argc;
    int status = 0;

    /* App-tab routing: while makmux is running (any VT child registered),
     * a fullscreen app opens in its own named tab (switch-if-exists handled
     * kernel-side) rather than replacing the current VT's shell view. */
    if (is_tab_app(argv[0]) && (sys_vt_state() & 0xFFFFu)) {
        char appp[VFS_PATH_MAX];
        if (resolve_app_path(argv[0], appp, sizeof(appp))) {
            sys_vt_open_app(appp);
            return 0;
        }
    }

    if (looks_like_path(argv[0])) {
        if (try_exec_path(argv[0], argv, &status)) return status;
        put_s("Unknown command '"); put_s(argv[0]); put_s("' - try 'lsman'.\n");
        return 127;
    }
    char dir_buf[VFS_PATH_MAX], path_buf[VFS_PATH_MAX];
    for (int p = 0; shell_path_dir(p, dir_buf, sizeof(dir_buf)); p++) {
        unsigned int dl = s_len(dir_buf);
        unsigned int nl = s_len(argv[0]);
        if (dl + nl + 1 >= VFS_PATH_MAX) continue;
        unsigned int i;
        for (i = 0; i < dl; i++) path_buf[i] = dir_buf[i];
        for (unsigned int j = 0; j < nl; j++) path_buf[dl + j] = argv[0][j];
        path_buf[dl + nl] = '\0';
        if (try_exec_path(path_buf, argv, &status)) return status;
    }
    if (is_makbox_applet(argv[0])) {
        static char makbox_argv0[] = "makbox";
        char *new_argv[MAX_ARGS + 1];
        int new_argc = 0;
        new_argv[new_argc++] = makbox_argv0;
        for (int i = 0; i < argc && new_argc < MAX_ARGS; i++) new_argv[new_argc++] = argv[i];
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
    put_s("Unknown command '"); put_s(argv[0]); put_s("' - try 'lsman'.\n");
    return 127;
}

/* ---------- redirection support ---------- */

/* Parsed redirection: open `path` with `flags`, then dup2 onto `target_fd`. */
typedef struct {
    int   target_fd;
    int   flags;
    char *path;
} redirect_t;

#define MAX_REDIRECTS 4

/* Scan argv for `<`, `>`, `>>`, `2>`, `2>>`.  Pairs of operator + filename
 * are extracted into out_red[] and the argv is compacted in place to drop
 * them.  Returns the new argc.  Sets *n_red.  Treats unrecognised forms
 * (e.g. `>` with no following arg) as literals and leaves them alone. */
static int extract_redirects(int argc, char **argv, redirect_t *out_red, int *n_red)
{
    int nr = 0;
    int o = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        int  tgt = -1;
        int  flg = 0;
        int  skip_next = 0;
        if (s_eq(a, "<")) {
            tgt = 0; flg = O_RDONLY; skip_next = 1;
        } else if (s_eq(a, ">")) {
            tgt = 1; flg = O_WRONLY | O_CREAT | O_TRUNC; skip_next = 1;
        } else if (s_eq(a, ">>")) {
            tgt = 1; flg = O_WRONLY | O_CREAT | O_APPEND; skip_next = 1;
        } else if (s_eq(a, "2>")) {
            tgt = 2; flg = O_WRONLY | O_CREAT | O_TRUNC; skip_next = 1;
        } else if (s_eq(a, "2>>")) {
            tgt = 2; flg = O_WRONLY | O_CREAT | O_APPEND; skip_next = 1;
        }
        if (skip_next) {
            if (i + 1 >= argc) {
                put_s("sh: syntax error: redirect missing filename\n");
                /* leave the partially-built redirect list intact; bad
                 * input simply collapses to the leftover argv we kept. */
                *n_red = nr;
                argv[o] = (char *)0;
                return o;
            }
            if (nr >= MAX_REDIRECTS) {
                put_s("sh: too many redirects\n");
                *n_red = nr;
                argv[o] = (char *)0;
                return o;
            }
            out_red[nr].target_fd = tgt;
            out_red[nr].flags     = flg;
            out_red[nr].path      = argv[i + 1];
            nr++;
            i++;   /* consume the filename arg */
            continue;
        }
        argv[o++] = argv[i];
    }
    argv[o] = (char *)0;
    *n_red = nr;
    return o;
}

/* Open each redirect's file and dup2 it onto its target fd.  Called from
 * the forked child only -- the parent never wants its own stdin/stdout
 * clobbered.  On open() failure, prints to stderr and sys_exit(1). */
static void apply_redirects(int n_red, const redirect_t *red)
{
    for (int i = 0; i < n_red; i++) {
        int fd = sys_open(red[i].path, red[i].flags);
        if (fd < 0) {
            put_s("sh: cannot open '");
            put_s(red[i].path);
            put_s("' for redirect\n");
            sys_exit(1);
        }
        if (fd != red[i].target_fd) {
            sys_dup2(fd, red[i].target_fd);
            sys_close(fd);
        }
    }
}

/* Pending redirect state -- set by run_line for a single command, applied
 * by spawn()'s forked child.  Pipelines stash their own per-stage redirects
 * on the stack, so they don't touch this. */
static redirect_t g_pending_red[MAX_REDIRECTS];
static int        g_pending_red_n = 0;

static void set_pending_redirects(int n, const redirect_t *r)
{
    g_pending_red_n = n;
    for (int i = 0; i < n; i++) g_pending_red[i] = r[i];
}

static void apply_pending_redirects_in_child(void)
{
    apply_redirects(g_pending_red_n, g_pending_red);
}

/* ---------- pipeline support ---------- */

/* Re-implement run_external without the fork wrapper -- the caller is
 * already inside a forked child, so it should `execve` straight on top of
 * itself.  sys_exit(127) on failure (POSIX "command not found"). */
static void child_exec_external(int argc, char **argv)
{
    if (argc == 0) sys_exit(127);
    if (looks_like_path(argv[0])) {
        sys_execve(argv[0], argv, (char *const *)0);
        char wext[VFS_PATH_MAX];
        unsigned int plen = s_len(argv[0]);
        if (plen + 5 < VFS_PATH_MAX) {
            unsigned int i;
            for (i = 0; i < plen; i++) wext[i] = argv[0][i];
            wext[i++] = '.'; wext[i++] = 'e'; wext[i++] = 'l'; wext[i++] = 'f'; wext[i] = '\0';
            sys_execve(wext, argv, (char *const *)0);
        }
        sys_exit(127);
    }
    char dir_buf[VFS_PATH_MAX], path_buf[VFS_PATH_MAX];
    for (int p = 0; shell_path_dir(p, dir_buf, sizeof(dir_buf)); p++) {
        unsigned int dl = s_len(dir_buf);
        unsigned int nl = s_len(argv[0]);
        if (dl + nl + 1 >= VFS_PATH_MAX) continue;
        unsigned int i;
        for (i = 0; i < dl; i++) path_buf[i] = dir_buf[i];
        for (unsigned int j = 0; j < nl; j++) path_buf[dl + j] = argv[0][j];
        path_buf[dl + nl] = '\0';
        if (path_exists(path_buf)) {
            sys_execve(path_buf, argv, (char *const *)0);
        }
    }
    if (is_makbox_applet(argv[0])) {
        static char makbox_argv0[] = "makbox";
        char *new_argv[MAX_ARGS + 1];
        int new_argc = 0;
        new_argv[new_argc++] = makbox_argv0;
        for (int i = 0; i < argc && new_argc < MAX_ARGS; i++) new_argv[new_argc++] = argv[i];
        new_argv[new_argc] = (char *)0;
        for (int p = 0; shell_path_dir(p, dir_buf, sizeof(dir_buf)); p++) {
            unsigned int dl = s_len(dir_buf);
            if (dl + sizeof(makbox_argv0) >= VFS_PATH_MAX) continue;
            unsigned int i;
            for (i = 0; i < dl; i++) path_buf[i] = dir_buf[i];
            for (i = 0; makbox_argv0[i]; i++) path_buf[dl + i] = makbox_argv0[i];
            path_buf[dl + i] = '\0';
            if (path_exists(path_buf)) {
                sys_execve(path_buf, new_argv, (char *const *)0);
            }
        }
    }
    sys_exit(127);
}

/* Split `s` in-place on top-level `|` characters (treating quoted regions
 * as opaque).  Writes a NUL at each pipe and stores the resulting stage
 * pointers into `out`.  Returns the number of stages. */
static int split_pipes(char *s, char **out, int max_stages)
{
    int n = 0;
    int in_single = 0, in_double = 0;
    out[n++] = s;
    for (char *p = s; *p; p++) {
        if (*p == '\'' && !in_double) { in_single = !in_single; continue; }
        if (*p == '"'  && !in_single) { in_double = !in_double; continue; }
        if (*p == '|' && !in_single && !in_double) {
            *p = '\0';
            if (n >= max_stages) return n;
            out[n++] = p + 1;
        }
    }
    return n;
}

/* Run a multi-stage pipeline.  Each stage is a (mutable) string we
 * tokenize per child.  Stage i's stdout is wired to stage i+1's stdin
 * via a fresh pipe.  Returns the last stage's exit status.
 *
 * Cap stages at 8 -- enough for real shell use, keeps the bookkeeping
 * arrays small. */
#define PIPE_MAX_STAGES 8
static int run_pipeline(int n_stages, char *stages[])
{
    if (n_stages > PIPE_MAX_STAGES) {
        put_s("sh: pipeline too deep\n");
        return 1;
    }
    int pids[PIPE_MAX_STAGES];
    int prev_read = -1;       /* read end of the previous stage's pipe */
    int next_pipe[2];

    for (int s = 0; s < n_stages; s++) {
        int has_next = (s + 1 < n_stages);
        if (has_next) {
            if (sys_pipe(next_pipe) != 0) {
                put_s("sh: pipe failed\n");
                if (prev_read >= 0) sys_close(prev_read);
                /* Reap any already-forked stages so they don't dangle. */
                for (int k = 0; k < s; k++) {
                    int st = 0; sys_wait4(pids[k], &st, 0);
                }
                return 1;
            }
        }
        int pid = sys_fork();
        if (pid < 0) {
            put_s("sh: fork failed\n");
            if (prev_read >= 0) sys_close(prev_read);
            if (has_next) { sys_close(next_pipe[0]); sys_close(next_pipe[1]); }
            return 1;
        }
        if (pid == 0) {
            /* Child: wire stdin from prev_read, stdout into next_pipe[1]. */
            if (prev_read >= 0) {
                sys_dup2(prev_read, 0);
                sys_close(prev_read);
            }
            if (has_next) {
                sys_dup2(next_pipe[1], 1);
                sys_close(next_pipe[0]);
                sys_close(next_pipe[1]);
            }
            /* Tokenize this stage's string and exec. */
            char *args[MAX_ARGS];
            int ac = tokenize(stages[s], args);
            if (ac == 0) sys_exit(0);
            static char glob_storage[2048];
            ac = expand_globs(ac, args, MAX_ARGS, glob_storage, sizeof(glob_storage));
            /* Per-stage redirects (e.g. `cmd1 > file | cmd2`).  Applied
             * after the pipe-fd dup2s so an explicit `>` overrides the
             * pipe wiring -- POSIX semantics. */
            redirect_t red[MAX_REDIRECTS];
            int n_red = 0;
            ac = extract_redirects(ac, args, red, &n_red);
            apply_redirects(n_red, red);
            child_exec_external(ac, args);
        }
        /* Parent: advance pipe bookkeeping. */
        pids[s] = pid;
        if (prev_read >= 0) sys_close(prev_read);
        if (has_next) {
            sys_close(next_pipe[1]);
            prev_read = next_pipe[0];
        } else {
            prev_read = -1;
        }
    }
    int status = 0;
    int last_status = 0;
    for (int s = 0; s < n_stages; s++) {
        int st = 0;
        sys_wait4(pids[s], &st, 0);
        if (s == n_stages - 1) last_status = st & 0xFF;
        (void)status;
    }
    return last_status;
}

/* ---------- single-segment dispatch ----------
 *
 * Runs one segment of a `&&` / `||` / `&` list.  Handles assignment,
 * pipelines (`|`), builtins / admin / external commands, and per-segment
 * redirects.  Updates g_last_status.  `segment` is a mutable, already-
 * variable-expanded string (the list walker handles expansion once at
 * the top of run_line for the whole line). */
static int run_one_segment(char *segment, int *should_exit, int *exit_status)
{
    /* Strip leading whitespace + skip comments / empty lines. */
    while (*segment == ' ' || *segment == '\t') segment++;
    if (*segment == '\0' || *segment == '#') return 0;

    /* Standalone assignment: NAME=VAL */
    const char *eq = assign_eq(segment);
    if (eq) {
        char name[VAR_NAME_MAX];
        int nlen = (int)(eq - segment);
        if (nlen >= VAR_NAME_MAX) nlen = VAR_NAME_MAX - 1;
        for (int i = 0; i < nlen; i++) name[i] = segment[i];
        name[nlen] = '\0';
        /* RHS already variable-expanded by run_line. */
        g_last_status = (var_set(name, eq + 1) == 0) ? 0 : 1;
        return 0;
    }

    /* Pipeline?  Split on top-level `|` and hand off to run_pipeline. */
    {
        char pipe_buf[LINE_MAX];
        s_copy(pipe_buf, segment, sizeof(pipe_buf));
        char *stages[PIPE_MAX_STAGES];
        int n = split_pipes(pipe_buf, stages, PIPE_MAX_STAGES);
        if (n > 1) {
            g_last_status = run_pipeline(n, stages);
            return 0;
        }
    }

    char *args[MAX_ARGS];
    int ac = tokenize(segment, args);
    if (ac == 0) return 0;

    /* Glob expansion (storage in static scratch). */
    static char glob_storage[2048];
    ac = expand_globs(ac, args, MAX_ARGS, glob_storage, sizeof(glob_storage));

    /* Strip redirects out of argv; spawn() will apply them post-fork. */
    redirect_t red[MAX_REDIRECTS];
    int n_red = 0;
    ac = extract_redirects(ac, args, red, &n_red);
    set_pending_redirects(n_red, red);

    if (run_builtin(ac, args, should_exit, exit_status)) goto out;
    if (run_admin(ac, args)) goto out;
    g_last_status = run_external(ac, args);
out:
    set_pending_redirects(0, (const redirect_t *)0);
    return 0;
}

/* ---------- background-job table ----------
 *
 * `cmd &` forks and records the pid here; `wait` builtin drains the
 * table by wait4()-ing each entry.  Cap kept small (POSIX users rarely
 * need >8 concurrent background jobs in a shell session). */
#define JOBS_MAX 16
static int g_jobs[JOBS_MAX];
static int g_jobs_n = 0;

static void jobs_add(int pid)
{
    if (g_jobs_n < JOBS_MAX) g_jobs[g_jobs_n++] = pid;
}

/* `wait` builtin: drain all backgrounded jobs.  Returns the last
 * reaped child's exit status (POSIX leaves this implementation-defined
 * when `wait` has no args -- the last is the simplest sensible pick). */
static int jobs_wait_all(void)
{
    int last = 0;
    for (int i = 0; i < g_jobs_n; i++) {
        int st = 0;
        sys_wait4(g_jobs[i], &st, 0);
        last = st & 0xFF;
    }
    g_jobs_n = 0;
    return last;
}

/* ---------- list-level dispatch (run_line) ---------- */

/* Operator between two segments. */
enum { LIST_SEP_NONE = 0, LIST_SEP_AND, LIST_SEP_OR, LIST_SEP_BG };

/* Walks `buf` (mutated in place), splitting at top-level `&&`, `||`, `&`
 * (outside quotes).  Each segment's left separator is taken from the
 * previous iteration's right separator.  Calls run_one_segment on each
 * segment that the gate allows. */
static int run_line(const char *raw, int *should_exit, int *exit_status)
{
    /* Skip leading whitespace + comment-only lines fast. */
    while (*raw == ' ' || *raw == '\t') raw++;
    if (*raw == '\0' || *raw == '#') return 0;

    /* Expand $VAR/$? once across the whole line so segment-level work
     * doesn't have to re-derive values. */
    char expanded[LINE_MAX];
    expand(raw, expanded, sizeof(expanded));

    int prev_sep = LIST_SEP_NONE;
    int prev_status = 0;
    char *p = expanded;
    while (*p) {
        /* Find this segment's right boundary. */
        char *seg = p;
        int next_sep = LIST_SEP_NONE;
        int sq = 0, dq = 0;
        while (*p) {
            if (*p == '\'' && !dq) { sq = !sq; p++; continue; }
            if (*p == '"'  && !sq) { dq = !dq; p++; continue; }
            if (!sq && !dq) {
                if (p[0] == '&' && p[1] == '&') {
                    next_sep = LIST_SEP_AND; *p = '\0'; p += 2; break;
                }
                if (p[0] == '|' && p[1] == '|') {
                    next_sep = LIST_SEP_OR;  *p = '\0'; p += 2; break;
                }
                if (p[0] == '&' && p[1] != '&') {
                    next_sep = LIST_SEP_BG;  *p = '\0'; p += 1; break;
                }
                /* A lone `|` is a pipe; not a list operator -- skip past
                 * it (the segment includes the pipe, which run_one_segment
                 * splits on internally). */
            }
            p++;
        }

        /* Trim leading whitespace on the segment. */
        while (*seg == ' ' || *seg == '\t') seg++;

        /* Decide whether to run this segment. */
        int gate_open = 1;
        if (prev_sep == LIST_SEP_AND && prev_status != 0) gate_open = 0;
        else if (prev_sep == LIST_SEP_OR  && prev_status == 0) gate_open = 0;

        if (gate_open && *seg) {
            if (next_sep == LIST_SEP_BG) {
                /* Background: fork; child runs the segment + exits.
                 * Parent records the pid so `wait` can drain it. */
                int pid = sys_fork();
                if (pid < 0) {
                    put_s("sh: fork failed (bg)\n");
                    g_last_status = 1;
                } else if (pid == 0) {
                    int de = 0, dx = 0;
                    char seg_copy[LINE_MAX];
                    s_copy(seg_copy, seg, sizeof(seg_copy));
                    run_one_segment(seg_copy, &de, &dx);
                    sys_exit(g_last_status);
                } else {
                    jobs_add(pid);
                    g_last_status = 0;     /* `cmd &` exits 0 immediately */
                }
            } else {
                char seg_copy[LINE_MAX];
                s_copy(seg_copy, seg, sizeof(seg_copy));
                run_one_segment(seg_copy, should_exit, exit_status);
            }
            prev_status = g_last_status;
        }

        prev_sep = next_sep;
    }
    return 0;
}

/* ---------- script (multi-line) interpreter ----------
 *
 * Splits source into statements on `;` or newlines.  Recognises
 *   if COND; then BODY [; elif COND; then BODY] [; else BODY] ; fi
 *   while COND; do BODY ; done
 *   for VAR in WORDS; do BODY ; done
 * Bodies may contain multiple `;`-separated statements.  No nested
 * if-inside-if support yet (rare in shell scripts; user can structure
 * around it).  Returns final $? value. */

/* Statement boundary scan: walks until matching `;` or newline at depth 0
 * (we don't have real expression nesting, so depth tracking is symbolic
 * for if/done/fi -- this implementation is single-level).  Returns one
 * past the terminator. */

/* Read one logical line ending at ; or \n; copy into `out` minus the
 * delimiter; returns pointer past the delim, or NULL at end-of-source. */
static const char *next_statement(const char *src, char *out, unsigned int outsz)
{
    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == ';') src++;
    if (*src == '\0') return (const char *)0;
    unsigned int o = 0;
    while (*src && *src != '\n' && *src != ';' && o + 1 < outsz) {
        if (*src == '#') { while (*src && *src != '\n') src++; break; }
        out[o++] = *src++;
    }
    /* Trim trailing whitespace. */
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = '\0';
    while (*src == ';' || *src == '\n' || *src == ' ' || *src == '\t') src++;
    return src;
}

/* Parser context: we read statements one by one and dispatch.  for/if/
 * while consume their body statements until they see fi/done. */

typedef struct { const char *src; char pending[SCRIPT_LINE_MAX]; int has_pending; } parser_t;

static int run_block_until(parser_t *p, const char *terminator1,
                           const char *terminator2, const char *terminator3,
                           int execute);

/* Pending-aware statement reader.  When a control keyword is glued to an
 * inline body (`then echo x`, `else echo y`, `elif COND`), the trailing
 * body is stashed in p->pending by match_kw and handed back here on the
 * next read before we advance p->src again.  This is what lets the block
 * parser accept single-line `if COND; then BODY; else BODY; fi` in
 * addition to the multi-line standalone-keyword form.  Returns NULL only
 * at real end-of-source. */
static const char *rb_next(parser_t *p, char *out, unsigned int sz)
{
    if (p->has_pending) {
        s_copy(out, p->pending, sz);
        p->has_pending = 0;
        return p->src;   /* non-NULL sentinel; p->src already positioned */
    }
    const char *n = next_statement(p->src, out, sz);
    if (n) p->src = n;
    return n;
}

/* Match a control keyword that may carry an inline body.  Returns 1 when
 * `stmt` is exactly `kw` (standalone form) or begins with `kw ` (inline
 * form -- the trailing body is stashed in p->pending for the next
 * rb_next).  Returns 0 otherwise. */
static int match_kw(parser_t *p, const char *stmt, const char *kw)
{
    if (s_eq(stmt, kw)) return 1;
    unsigned int kl = s_len(kw);
    if (s_starts(stmt, kw) && stmt[kl] == ' ') {
        const char *body = stmt + kl + 1;
        while (*body == ' ' || *body == '\t') body++;
        if (*body) { s_copy(p->pending, body, sizeof(p->pending)); p->has_pending = 1; }
        return 1;
    }
    return 0;
}

/* Remembers which terminator (fi/done/elif/else) the most recent
 * run_block_until() consumed, so the if/elif/else chain handler can
 * branch on it without re-tokenizing. */
static char g_last_terminator[32] = "";

/* run_block_until: read statements until one of terminator{1,2,3} (any may
 * be NULL).  If execute, dispatch each.  Returns 0 if hit terminator,
 * -1 on unexpected EOF.  Updates p->src past the terminator. */
static int run_block_until(parser_t *p, const char *t1, const char *t2, const char *t3,
                           int execute)
{
    char stmt[SCRIPT_LINE_MAX];
    int should_exit = 0, exit_status = 0;
    for (;;) {
        const char *next = rb_next(p, stmt, sizeof(stmt));
        if (!next) return -1;
        const char *term = (const char *)0;
        if      (t1 && match_kw(p, stmt, t1)) term = t1;
        else if (t2 && match_kw(p, stmt, t2)) term = t2;
        else if (t3 && match_kw(p, stmt, t3)) term = t3;
        if (term) {
            /* Remember which terminator: caller may distinguish via lookahead.
             * We stash it via a static (single-threaded interpreter). */
            s_copy(g_last_terminator, term, 32);
            return 0;
        }
        /* Sub-block keywords. */
        if (s_starts(stmt, "if ")) {
            int cond = 0;
            (void)run_line(stmt + 3, &should_exit, &exit_status);
            cond = (g_last_status == 0);
            /* Expect `then` next, then body until elif/else/fi.  Accept
             * both standalone `then` and inline `then BODY` (match_kw
             * stashes the body for the body block to pick up). */
            const char *then_stmt = rb_next(p, stmt, sizeof(stmt));
            if (!then_stmt || !match_kw(p, stmt, "then")) { put_s("sh: expected 'then'\n"); return -1; }
            int branch_taken = cond;
            int rc = run_block_until(p, "elif", "else", "fi", execute && branch_taken);
            if (rc < 0) return -1;
            while (s_eq(g_last_terminator, "elif")) {
                /* elif COND; then BODY ...  match_kw on the terminator
                 * already stashed COND as pending when the elif was glued
                 * inline, so rb_next yields it here. */
                const char *cond_stmt = rb_next(p, stmt, sizeof(stmt));
                if (!cond_stmt) return -1;
                int this_cond = 0;
                (void)run_line(stmt, &should_exit, &exit_status);
                this_cond = (g_last_status == 0);
                const char *then2 = rb_next(p, stmt, sizeof(stmt));
                if (!then2 || !match_kw(p, stmt, "then")) { put_s("sh: expected 'then'\n"); return -1; }
                int take = (!branch_taken) && this_cond;
                rc = run_block_until(p, "elif", "else", "fi", execute && take);
                if (rc < 0) return -1;
                if (take) branch_taken = 1;
            }
            if (s_eq(g_last_terminator, "else")) {
                int take = !branch_taken;
                rc = run_block_until(p, "fi", (const char *)0, (const char *)0, execute && take);
                if (rc < 0) return -1;
            }
            continue;
        }
        if (s_starts(stmt, "while ")) {
            const char *cond_text_start = p->src;  /* unused; we re-evaluate */
            (void)cond_text_start;
            /* We need to be able to re-run cond + body.  Snapshot cond text
             * (we already have it in stmt), and find the body span. */
            char cond_line[SCRIPT_LINE_MAX];
            s_copy(cond_line, stmt + 6, sizeof(cond_line));
            /* Expect `do`. */
            const char *do_stmt = rb_next(p, stmt, sizeof(stmt));
            if (!do_stmt || !match_kw(p, stmt, "do")) { put_s("sh: expected 'do'\n"); return -1; }
            const char *body_start = p->src;
            int entered = 0;
            for (int iter = 0; iter < 100000; iter++) {
                (void)run_line(cond_line, &should_exit, &exit_status);
                if (g_last_status != 0) break;
                entered = 1;
                p->src = body_start;
                int rc = run_block_until(p, "done", (const char *)0, (const char *)0, execute);
                if (rc < 0) return -1;
            }
            if (!entered) {
                /* Cond was false from the start; still need to consume body. */
                p->src = body_start;
                int rc = run_block_until(p, "done", (const char *)0, (const char *)0, 0);
                if (rc < 0) return -1;
            }
            continue;
        }
        if (s_starts(stmt, "for ")) {
            /* for VAR in W1 W2 ... ; do BODY ; done */
            char head[SCRIPT_LINE_MAX]; s_copy(head, stmt + 4, sizeof(head));
            char *vname = head;
            char *p_in = head;
            while (*p_in && *p_in != ' ' && *p_in != '\t') p_in++;
            if (*p_in) { *p_in = '\0'; p_in++; }
            while (*p_in == ' ' || *p_in == '\t') p_in++;
            if (!(p_in[0] == 'i' && p_in[1] == 'n' &&
                 (p_in[2] == ' ' || p_in[2] == '\t' || p_in[2] == '\0'))) {
                put_s("sh: for: expected 'in'\n"); return -1;
            }
            p_in += 2;
            while (*p_in == ' ' || *p_in == '\t') p_in++;
            /* Expand vars in word list. */
            char wlist[SCRIPT_LINE_MAX];
            expand(p_in, wlist, sizeof(wlist));
            /* Expect `do`. */
            const char *do_stmt = rb_next(p, stmt, sizeof(stmt));
            if (!do_stmt || !match_kw(p, stmt, "do")) { put_s("sh: for: expected 'do'\n"); return -1; }
            const char *body_start = p->src;
            /* Iterate words. */
            char *wp = wlist;
            for (;;) {
                while (*wp == ' ' || *wp == '\t') wp++;
                if (!*wp) break;
                char *word = wp;
                while (*wp && *wp != ' ' && *wp != '\t') wp++;
                char sav = *wp;
                if (*wp) *wp = '\0';
                var_set(vname, word);
                if (sav) *wp = sav;
                p->src = body_start;
                int rc = run_block_until(p, "done", (const char *)0, (const char *)0, execute);
                if (rc < 0) return -1;
                if (*wp) wp++;
            }
            /* If no words, still consume body. */
            if (!*wlist) {
                p->src = body_start;
                int rc = run_block_until(p, "done", (const char *)0, (const char *)0, 0);
                if (rc < 0) return -1;
            }
            continue;
        }
        /* Plain statement. */
        if (execute) {
            int se = 0, es = 0;
            run_line(stmt, &se, &es);
            if (se) { exit_status = es; should_exit = 1; }
        }
    }
}

static int run_script_buf(const char *src)
{
    parser_t p = {0}; p.src = src;
    char stmt[SCRIPT_LINE_MAX];
    int should_exit = 0, exit_status = 0;
    for (;;) {
        const char *next = next_statement(p.src, stmt, sizeof(stmt));
        if (!next) break;
        p.src = next;
        if (s_starts(stmt, "if ") || s_starts(stmt, "while ") || s_starts(stmt, "for ")) {
            /* Re-enter via run_block_until pretending we're inside a block
             * terminated by EOF.  The block parser handles each form. */
            /* Push the statement back and call run_block_until with NULL
             * terminators so we run until EOF. */
            /* Simpler: re-create a parser starting at this statement. */
            unsigned int slen = s_len(stmt);
            /* Pseudo-source: stmt + ";" + remaining src. */
            static char rebuilt[8192];
            unsigned int o = 0;
            for (unsigned int i = 0; i < slen && o + 1 < sizeof(rebuilt); i++) rebuilt[o++] = stmt[i];
            if (o + 1 < sizeof(rebuilt)) rebuilt[o++] = ';';
            for (unsigned int i = 0; p.src[i] && o + 1 < sizeof(rebuilt); i++) rebuilt[o++] = p.src[i];
            rebuilt[o] = '\0';
            parser_t pp = {0}; pp.src = rebuilt;
            (void)run_block_until(&pp, (const char *)0, (const char *)0, (const char *)0, 1);
            break;
        }
        run_line(stmt, &should_exit, &exit_status);
        if (should_exit) break;
    }
    return g_last_status;
}

/* ---------- prompt ---------- */

static void build_prompt(char *out, unsigned int outsz)
{
    /* "logged in as" is global kernel state (auth_current_user); read it live
     * so a login that happened elsewhere -- notably the GUI graphical login,
     * which sets the session user via SYS_LOGIN after this shell started with
     * an empty --user= -- is reflected in the prompt (was showing `@host`). */
    { char wn[USER_MAX]; if (sys_whoami(wn, sizeof wn) > 0 && wn[0]) s_copy(g_username, wn, sizeof g_username); }
    char cwd[VFS_PATH_MAX];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) { cwd[0] = '/'; cwd[1] = '\0'; }
    /* user@host:cwd$  (root gets '#', unprivileged users get '$' -- the
     * usual sh convention so you can tell at a glance whether you're root). */
    unsigned int o = 0;
    for (unsigned int i = 0; g_username[i] && o + 1 < outsz; i++) out[o++] = g_username[i];
    if (o + 1 < outsz) out[o++] = '@';
    for (unsigned int i = 0; g_hostname[i] && o + 1 < outsz; i++) out[o++] = g_hostname[i];
    if (o + 1 < outsz) out[o++] = ':';
    for (unsigned int i = 0; cwd[i] && o + 1 < outsz; i++) out[o++] = cwd[i];
    if (o + 1 < outsz) out[o++] = s_eq(g_username, "root") ? '#' : '$';
    if (o + 1 < outsz) out[o++] = ' ';
    out[o] = '\0';
}

/* ---------- main REPL ---------- */

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    const char *script_path = (const char *)0;
    const char *cmd_string  = (const char *)0;   /* sh -c "<string>" */
    for (int i = 1; i < argc; i++) {
        if (s_eq(argv[i], "--login")) g_login = 1;
        else if (s_eq(argv[i], "--autostart=gui")) g_autostart_gui = 1;
        else if (s_eq(argv[i], "--autostart=gui-login")) g_autostart_gui = 2;
        else if (s_eq(argv[i], "--makmux")) g_quiet_start = 1;
        else if (s_eq(argv[i], "-c")) {
            /* Consume the next arg as the command string so it isn't
             * mistaken for a script path below.  Backs libc system(3). */
            if (i + 1 < argc) cmd_string = argv[++i];
        }
        else if (s_starts(argv[i], "--user="))
            s_copy(g_username, argv[i] + 7, sizeof(g_username));
        else if (argv[i][0] != '-')   script_path = argv[i];
    }

    if (g_login)
        ignore_login_shell_signals();

    /* Resolve hostname (best-effort). */
    char hbuf[HOST_MAX];
    int hn = sys_gethostname(hbuf, sizeof(hbuf));
    if (hn > 0) s_copy(g_hostname, hbuf, sizeof(g_hostname));

    /* Set HOME based on username: root -> /root, others -> /home/<user>. */
    {
        static char home_val[VFS_PATH_MAX];
        if (s_eq(g_username, "root")) {
            s_copy(home_val, "/root", sizeof(home_val));
        } else {
            home_val[0] = '/'; home_val[1] = 'h'; home_val[2] = 'o';
            home_val[3] = 'm'; home_val[4] = 'e'; home_val[5] = '/';
            s_copy(home_val + 6, g_username, sizeof(home_val) - 6);
        }
        var_set("HOME", home_val);

        /* A login shell starts in the user's home directory (the cwd we
         * inherit from the kernel login loop is '/').  Stat first so we
         * never hand SYS_CHDIR a path that doesn't exist -- the kernel's
         * vfs_cd prints "cd: directory not found" on a miss, which would
         * spew on a live session whose read-only rootfs has no
         * /home/<user>.  If HOME isn't a directory we just stay at '/'. */
        if (g_login) {
            struct stat hst;
            if (sys_stat(home_val, &hst) == 0 && S_ISDIR(hst.st_mode))
                (void)sys_chdir(home_val);
        }
    }

    /* Source ~/.makshrc on login shells (best-effort; missing file is fine). */
    if (g_login) {
        const char *home = var_get("HOME");
        if (home && *home) {
            static char rc_path[VFS_PATH_MAX];
            unsigned int hl = s_len(home);
            unsigned int i;
            for (i = 0; i < hl && i < sizeof(rc_path)-10; i++) rc_path[i] = home[i];
            rc_path[i++]='/'; rc_path[i++]='.'; rc_path[i++]='m';
            rc_path[i++]='a'; rc_path[i++]='k'; rc_path[i++]='s';
            rc_path[i++]='h'; rc_path[i++]='r'; rc_path[i++]='c';
            rc_path[i] = '\0';
            int rcfd = sys_open(rc_path, O_RDONLY);
            if (rcfd >= 0) {
                static char rcbuf[4096];
                int rd = sys_read(rcfd, rcbuf, sizeof(rcbuf) - 1);
                sys_close(rcfd);
                if (rd > 0) { rcbuf[rd] = '\0'; run_script_buf(rcbuf); }
            }
        }
    }

    /* Non-interactive: `-c "<string>"` runs the string and exits with $?. */
    if (cmd_string) {
        return run_script_buf(cmd_string);
    }

    /* Non-interactive: run script and exit. */
    if (script_path) {
        int fd = sys_open(script_path, O_RDONLY);
        if (fd < 0) { put_s("sh: cannot open "); put_s(script_path); put_c('\n'); return 1; }
        static char sbuf[16384];
        int rd = sys_read(fd, sbuf, sizeof(sbuf) - 1);
        sys_close(fd);
        if (rd < 0) return 1;
        sbuf[rd] = '\0';
        return run_script_buf(sbuf);
    }

    if (!g_login && !g_quiet_start) {
        put_s("sh.elf: ring-3 shell (Ctrl-D or `exit` to quit)\n");
    }

    /* autoboot=gui: launch the desktop now, exactly as if the user had typed
     * `gui`.  It spawns in the background (is_gui_path) so this login session
     * stays alive as the GUI's parent and resumes the prompt on Log Off. */
    if (g_autostart_gui == 1)      run_script_buf("gui\n");
    else if (g_autostart_gui == 2) run_script_buf("gui login\n");

    char line[LINE_MAX];
    char prompt[VFS_PATH_MAX + 64];

    for (;;) {
        unsigned int pos = sys_cursor_pos();
        if (((pos >> 16) & 0xFFFFu) != 0)
            put_c('\n');

        /* Sync marker for the in-guest test drivers -- the kernel-shell
         * REPL emits an identical [shell:ready vt=N] line before each
         * prompt, so existing scenarios work unchanged.  No-op unless
         * g_serial_verbose is on (kernel-side gate). */
        sys_shell_ready();
        build_prompt(prompt, sizeof(prompt));
        int n = readline(prompt, line);
        if (n == -1) {
            put_c('\n');
            if (g_login) { put_s("(use `shutdown` or `reboot` to power off)\n"); continue; }
            break;
        }
        if (n == -2) continue;
        if (n > 0) hist_push(line);

        int should_exit = 0;
        int exit_status = 0;
        /* If the line starts a control construct, route through the script
         * interpreter so multiline ;-split forms work at the REPL too. */
        if (s_starts(line, "if ") || s_starts(line, "while ") || s_starts(line, "for ")) {
            run_script_buf(line);
        } else {
            run_line(line, &should_exit, &exit_status);
        }
        if (should_exit) return exit_status;
    }

    return 0;
}
