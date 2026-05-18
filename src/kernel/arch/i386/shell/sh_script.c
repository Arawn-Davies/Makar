/*
 * sh_script.c - per-shell-task variable table + script interpreter.
 *
 * Variables live in task_t.script_vars (per-shell-task isolation,
 * Linux-VT-like).  Allocated on first set; grown by power-of-two.
 *
 * Script execution is line-oriented with a small recursive-descent
 * interpreter for if/while/for and a simple `[ ... ]` test builtin.
 * No subshells, no pipes, no command substitution -- we run inside
 * the kernel shell task, calling shell_dispatch for each statement.
 */

#include <kernel/sh_script.h>
#include <kernel/shell.h>
#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <string.h>
#include "shell_priv.h"

/* ---- variable table ------------------------------------------------------ */

typedef struct sh_var {
    char *name;
    char *value;
} sh_var_t;

typedef struct sh_var_tab {
    int       cap;
    int       len;
    sh_var_t *items;
} sh_var_tab_t;

static sh_var_tab_t *vars_for_current(int create)
{
    task_t *t = task_current();
    if (!t) return NULL;
    if (!t->script_vars && create) {
        sh_var_tab_t *tab = (sh_var_tab_t *)kmalloc(sizeof(*tab));
        if (!tab) return NULL;
        tab->cap = 0; tab->len = 0; tab->items = NULL;
        t->script_vars = tab;
    }
    return (sh_var_tab_t *)t->script_vars;
}

static int vars_find(sh_var_tab_t *tab, const char *name)
{
    if (!tab) return -1;
    for (int i = 0; i < tab->len; i++)
        if (strcmp(tab->items[i].name, name) == 0)
            return i;
    return -1;
}

const char *sh_vars_get(const char *name)
{
    sh_var_tab_t *tab = vars_for_current(0);
    int i = vars_find(tab, name);
    return (i >= 0) ? tab->items[i].value : NULL;
}

int sh_vars_set(const char *name, const char *value)
{
    sh_var_tab_t *tab = vars_for_current(1);
    if (!tab) return -1;
    int i = vars_find(tab, name);
    if (i >= 0) {
        char *nv = (char *)kmalloc(strlen(value) + 1);
        if (!nv) return -1;
        strcpy(nv, value);
        kfree(tab->items[i].value);
        tab->items[i].value = nv;
        return 0;
    }
    if (tab->len == tab->cap) {
        int   nc = tab->cap ? tab->cap * 2 : 8;
        sh_var_t *na = (sh_var_t *)kmalloc((size_t)nc * sizeof(sh_var_t));
        if (!na) return -1;
        if (tab->items) {
            memcpy(na, tab->items, (size_t)tab->len * sizeof(sh_var_t));
            kfree(tab->items);
        }
        tab->items = na;
        tab->cap   = nc;
    }
    char *nn = (char *)kmalloc(strlen(name)  + 1);
    char *nv = (char *)kmalloc(strlen(value) + 1);
    if (!nn || !nv) { kfree(nn); kfree(nv); return -1; }
    strcpy(nn, name); strcpy(nv, value);
    tab->items[tab->len].name  = nn;
    tab->items[tab->len].value = nv;
    tab->len++;
    return 0;
}

void sh_vars_unset(const char *name)
{
    sh_var_tab_t *tab = vars_for_current(0);
    int i = vars_find(tab, name);
    if (i < 0) return;
    kfree(tab->items[i].name);
    kfree(tab->items[i].value);
    tab->items[i] = tab->items[tab->len - 1];
    tab->len--;
}

void sh_vars_iter(sh_vars_iter_cb cb, void *ctx)
{
    sh_var_tab_t *tab = vars_for_current(0);
    if (!tab) return;
    for (int i = 0; i < tab->len; i++)
        if (cb(tab->items[i].name, tab->items[i].value, ctx))
            return;
}

void sh_vars_free_for(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->script_vars) return;
    sh_var_tab_t *tab = (sh_var_tab_t *)t->script_vars;
    for (int i = 0; i < tab->len; i++) {
        kfree(tab->items[i].name);
        kfree(tab->items[i].value);
    }
    kfree(tab->items);
    kfree(tab);
    t->script_vars = NULL;
}

/* ---- helpers ------------------------------------------------------------- */

static int is_ident_start(char c) { return (c=='_') || (c>='A'&&c<='Z') || (c>='a'&&c<='z'); }
static int is_ident_cont(char c)  { return is_ident_start(c) || (c>='0'&&c<='9'); }

/* ---- $VAR / ${VAR} expansion --------------------------------------------- */

int sh_expand(const char *in, char *out, size_t outsz)
{
    size_t oi = 0;
    for (size_t i = 0; in[i]; ) {
        /* $? -- last command's exit status.  Special single-char var,
         * not an identifier, so handle before the general ident path. */
        if (in[i] == '$' && in[i+1] == '?') {
            const char *v = sh_vars_get("?");
            if (!v) v = "0";
            size_t vl = strlen(v);
            if (oi + vl >= outsz) return -1;
            memcpy(out + oi, v, vl);
            oi += vl;
            i += 2;
            continue;
        }
        if (in[i] == '$' && (is_ident_start(in[i+1]) || in[i+1] == '{')) {
            int braced = (in[i+1] == '{');
            size_t s = i + 1 + (size_t)braced;
            size_t e = s;
            while (in[e] && is_ident_cont(in[e])) e++;
            if (e == s) { /* not actually a var */ goto literal; }
            if (braced && in[e] != '}') return -1;
            char name[64];
            size_t nl = e - s;
            if (nl >= sizeof(name)) return -1;
            memcpy(name, in + s, nl);
            name[nl] = '\0';
            const char *v = sh_vars_get(name);
            if (v) {
                size_t vl = strlen(v);
                if (oi + vl >= outsz) return -1;
                memcpy(out + oi, v, vl);
                oi += vl;
            }
            i = e + (size_t)braced;
            continue;
        }
literal:
        if (oi + 1 >= outsz) return -1;
        out[oi++] = in[i++];
    }
    out[oi] = '\0';
    return 0;
}

/* ---- assignment ---------------------------------------------------------- */

int sh_try_assign(const char *line)
{
    /* Match ^[A-Za-z_][A-Za-z0-9_]*=.*$ -- no leading whitespace, no spaces
     * around '='.  Matches bash/sh assignment grammar. */
    if (!is_ident_start(line[0])) return 0;
    size_t i = 1;
    while (is_ident_cont(line[i])) i++;
    if (line[i] != '=') return 0;
    char name[64];
    if (i >= sizeof(name)) return 0;
    memcpy(name, line, i);
    name[i] = '\0';
    const char *rhs = line + i + 1;
    static char expanded[512];
    if (sh_expand(rhs, expanded, sizeof(expanded)) != 0) {
        t_writestring("sh: assignment expansion overflow\n");
        return 1;
    }
    if (sh_vars_set(name, expanded) != 0)
        t_writestring("sh: OOM on assignment\n");
    return 1;
}

/* ---- comment strip ------------------------------------------------------- */

void sh_strip_comment(char *line)
{
    int sq = 0, dq = 0;
    for (size_t i = 0; line[i]; i++) {
        if (line[i] == '\'' && !dq) sq = !sq;
        else if (line[i] == '"' && !sq) dq = !dq;
        else if (line[i] == '#' && !sq && !dq) {
            line[i] = '\0';
            return;
        }
    }
}

/* ---- line execution ------------------------------------------------------ */

int sh_exec_line(char *line)
{
    /* Trim leading whitespace. */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return 1;
    sh_strip_comment(line);
    /* Trim trailing whitespace. */
    size_t l = strlen(line);
    while (l > 0 && (line[l-1] == ' ' || line[l-1] == '\t' || line[l-1] == '\r'))
        line[--l] = '\0';
    if (!*line) return 1;

    if (sh_try_assign(line)) return 1;

    static char expanded[1024];
    if (sh_expand(line, expanded, sizeof(expanded)) != 0) {
        t_writestring("sh: line too long after expansion\n");
        return 0;
    }
    char *argv[SHELL_MAX_ARGS];
    int argc = shell_parse(expanded, argv, SHELL_MAX_ARGS);
    if (argc == 0) return 1;
    return shell_dispatch_argv(argc, argv);
}

/* ---- script-file walker with if/while/for -------------------------------- */
/*
 * Strategy: load whole file into a buffer, split into NUL-terminated
 * lines (line[i] is a pointer into the buffer).  Walk with a small
 * stack-based interpreter.
 *
 * Supported constructs:
 *   if [ TEST ]; then ... elif [ TEST ]; then ... else ... fi
 *   while [ TEST ]; do ... done
 *   for VAR in WORDS; do ... done
 *   [ TEST ]          -- sets $? to 0/1 via _sh_last_status
 *
 * We split each line on `;` so the user can write `if [ x ]; then` on
 * one line, but we tolerate split across lines too.  Keywords are
 * recognised only when they are the first token of a logical statement.
 */

static int sh_last_status = 0;

/* Set $? from sh_last_status.  Called after every dispatched line, after
 * every `[ TEST ]` evaluation, and on exit from a script so the parent
 * shell observes the final status. */
static void publish_status(int rc)
{
    sh_last_status = rc;
    char buf[12];
    int n = 0;
    if (rc < 0) { buf[n++] = '-'; rc = -rc; }
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + (rc % 10)); rc /= 10; } while (rc && t < (int)sizeof(tmp));
    while (t > 0) buf[n++] = tmp[--t];
    buf[n] = '\0';
    sh_vars_set("?", buf);
}

/* Forward decl: defined further down. */
static int run_block(char **lines, int from, int to);

/* Find matching end-keyword (fi/done) starting from `from`, respecting
 * nesting.  Returns line index of the matching end, or -1. */
static int find_match(char **lines, int from, int to,
                      const char *open1, const char *open2,
                      const char *close, const char **mids, int n_mids,
                      int *first_mid_out)
{
    int depth = 1;
    if (first_mid_out) *first_mid_out = -1;
    for (int i = from; i < to; i++) {
        char *t = lines[i];
        while (*t == ' ' || *t == '\t') t++;
        size_t l1 = strlen(open1);
        if ((open2 && strncmp(t, open2, strlen(open2)) == 0 && (t[strlen(open2)] == ' ' || t[strlen(open2)] == '\t' || t[strlen(open2)] == '\0')) ||
            (strncmp(t, open1, l1) == 0 && (t[l1] == ' ' || t[l1] == '\t' || t[l1] == '\0'))) {
            depth++;
            continue;
        }
        size_t cl = strlen(close);
        if (strncmp(t, close, cl) == 0 && (t[cl] == '\0' || t[cl] == ' ' || t[cl] == ';')) {
            depth--;
            if (depth == 0) return i;
            continue;
        }
        if (depth == 1 && first_mid_out && *first_mid_out < 0) {
            for (int m = 0; m < n_mids; m++) {
                size_t ml = strlen(mids[m]);
                if (strncmp(t, mids[m], ml) == 0 &&
                    (t[ml] == '\0' || t[ml] == ' ' || t[ml] == ';')) {
                    *first_mid_out = i;
                    break;
                }
            }
        }
    }
    return -1;
}

/* `[ ARGS ]` test.  Supports:
 *   [ -z STR ]        empty
 *   [ -n STR ]        non-empty
 *   [ STR = STR ]     string equal
 *   [ STR != STR ]    string not equal
 *   [ INT -eq INT ]   integer equal (also -ne -lt -le -gt -ge)
 * Returns 0 on true, 1 on false (sh convention).
 */
static int is_numeric(const char *s)
{
    if (!s || !*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; s++; }
    return 1;
}

static int atoi_safe(const char *s) { int r=0,n=0; if(*s=='-'){n=1;s++;} else if(*s=='+'){s++;} while(*s>='0'&&*s<='9') r=r*10+(*s++-'0'); return n?-r:r; }

static int sh_test(int argc, char **argv)
{
    if (argc == 0) return 1;
    if (argc == 1) return (*argv[0]) ? 0 : 1;
    if (argc == 2) {
        if (strcmp(argv[0], "-z") == 0) return (!*argv[1]) ? 0 : 1;
        if (strcmp(argv[0], "-n") == 0) return ( *argv[1]) ? 0 : 1;
    }
    if (argc == 3) {
        if (strcmp(argv[1], "=")  == 0) return strcmp(argv[0], argv[2]) == 0 ? 0 : 1;
        if (strcmp(argv[1], "!=") == 0) return strcmp(argv[0], argv[2]) != 0 ? 0 : 1;
        /* Integer ops: refuse if either operand isn't numeric so a
         * non-numeric string doesn't silently compare as 0. */
        int is_int_op =
               (strcmp(argv[1], "-eq") == 0) || (strcmp(argv[1], "-ne") == 0)
            || (strcmp(argv[1], "-lt") == 0) || (strcmp(argv[1], "-le") == 0)
            || (strcmp(argv[1], "-gt") == 0) || (strcmp(argv[1], "-ge") == 0);
        if (is_int_op) {
            if (!is_numeric(argv[0]) || !is_numeric(argv[2])) {
                t_writestring("sh: [: integer expected\n");
                return 2;   /* bash convention: usage/operand error */
            }
            int a = atoi_safe(argv[0]), b = atoi_safe(argv[2]);
            if (strcmp(argv[1], "-eq") == 0) return a == b ? 0 : 1;
            if (strcmp(argv[1], "-ne") == 0) return a != b ? 0 : 1;
            if (strcmp(argv[1], "-lt") == 0) return a <  b ? 0 : 1;
            if (strcmp(argv[1], "-le") == 0) return a <= b ? 0 : 1;
            if (strcmp(argv[1], "-gt") == 0) return a >  b ? 0 : 1;
            if (strcmp(argv[1], "-ge") == 0) return a >= b ? 0 : 1;
        }
    }
    return 1;
}

/* Evaluate a `[ ... ]` condition expressed as a raw line.  Returns 1
 * (true) or 0 (false). */
static int eval_condition(const char *line)
{
    /* Strip leading whitespace + opening `[`. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '[') {
        /* Treat as a command; success means status 0. */
        static char buf[256];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        int rc = sh_exec_line(buf);
        return rc ? 1 : 0;   /* dispatched OK ~= "true" */
    }
    line++;   /* skip [ */
    static char buf[256];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    /* Strip trailing `]` and optional trailing `;` from a single-line form. */
    size_t l = strlen(buf);
    while (l > 0 && (buf[l-1] == ';' || buf[l-1] == ' ' || buf[l-1] == '\t')) buf[--l] = '\0';
    if (l > 0 && buf[l-1] == ']') buf[--l] = '\0';
    static char expanded[512];
    if (sh_expand(buf, expanded, sizeof(expanded)) != 0) return 0;
    char *argv[SHELL_MAX_ARGS];
    int argc = shell_parse(expanded, argv, SHELL_MAX_ARGS);
    int rc = sh_test(argc, argv);
    publish_status(rc);
    return rc == 0;
}

/* Run lines[from .. to-1] as a block, honouring keywords. */
static int run_block(char **lines, int from, int to)
{
    int i = from;
    while (i < to) {
        char *t = lines[i];
        while (*t == ' ' || *t == '\t') t++;

        /* Single-line form: `if [ X ]; then CMD; fi`.  Bash-compatible.
         * Detect by spotting `; fi` at the end of this line (after `; then`).
         * Handles only one body command (no elif/else on the same line). */
        if ((strncmp(t, "if ", 3) == 0 || strcmp(t, "if") == 0)) {
            size_t tl = strlen(t);
            int has_inline_fi = 0;
            if (tl >= 4) {
                /* Match ` fi` (with optional trailing whitespace already trimmed). */
                if (t[tl-2] == 'f' && t[tl-1] == 'i' &&
                    (tl == 2 || t[tl-3] == ' ' || t[tl-3] == ';'))
                    has_inline_fi = 1;
            }
            if (has_inline_fi) {
                /* Split into <cond> | <body>.  Layout:
                 *   if <COND>; then <BODY>; fi
                 * Steps: find " then " or ";then "; that splits cond/body.
                 * Then strip trailing "; fi" from body. */
                static char work[512];
                strncpy(work, t, sizeof(work) - 1);
                work[sizeof(work) - 1] = '\0';
                /* skip past "if " */
                char *cp = work + (work[2] == ' ' ? 3 : 2);
                while (*cp == ' ') cp++;
                char *then_kw = strstr(cp, "then");
                if (then_kw) {
                    /* Cond ends just before `then` (and any `;` / spaces). */
                    char *cend = then_kw;
                    while (cend > cp && (cend[-1] == ' ' || cend[-1] == ';' || cend[-1] == '\t'))
                        cend--;
                    *cend = '\0';
                    char *body = then_kw + 4;
                    while (*body == ' ') body++;
                    /* Trim trailing "; fi" (or " fi") from body. */
                    size_t bl = strlen(body);
                    while (bl > 0 && (body[bl-1] == ' ' || body[bl-1] == '\t')) body[--bl] = '\0';
                    if (bl >= 2 && body[bl-1] == 'i' && body[bl-2] == 'f') {
                        bl -= 2;
                        while (bl > 0 && (body[bl-1] == ' ' || body[bl-1] == ';')) bl--;
                        body[bl] = '\0';
                    }
                    if (eval_condition(cp)) {
                        sh_exec_line(body);
                    }
                    i++;
                    continue;
                }
                /* Falls through to multi-line path if no `then`. */
            }
            /* Multi-line form follows. */
        }

        if (strncmp(t, "if ", 3) == 0 || strcmp(t, "if") == 0) {
            /* Find the matching `fi`, ignoring nested if/fi.  We don't
             * use find_match's mid output for elif chaining -- we walk
             * each elif/else inline below so we visit them in order. */
            int end = find_match(lines, i + 1, to, "if", NULL, "fi",
                                 NULL, 0, NULL);
            if (end < 0) { t_writestring("sh: missing fi\n"); return -2; }

            /* `cond_line` points at the current branch's condition line
             * (the `if [ X ]; then` or `elif [ X ]; then` line).
             * `body_from` is the first line of that branch's body. */
            int cond_line  = i;
            const char *cond_kw = "if";
            size_t      kw_len  = 2;
            int rc = 0;
            int taken = 0;
            int scan = i + 1;

            while (1) {
                /* Extract this branch's condition from cond_line. */
                char *ct = lines[cond_line];
                while (*ct == ' ' || *ct == '\t') ct++;
                ct += kw_len;
                while (*ct == ' ' || *ct == '\t') ct++;
                static char cond[256];
                strncpy(cond, ct, sizeof(cond) - 1);
                cond[sizeof(cond) - 1] = '\0';
                char *th = strstr(cond, "then");
                if (th) {
                    while (th > cond && (th[-1] == ' ' || th[-1] == ';' || th[-1] == '\t'))
                        th--;
                    *th = '\0';
                }
                /* Find the next elif/else/fi at our depth so we know
                 * where this branch's body ends. */
                int depth = 1;
                int next  = end;   /* default: extends to `fi` */
                int next_is_else = 0;
                for (int k = scan; k < end; k++) {
                    char *kt = lines[k];
                    while (*kt == ' ' || *kt == '\t') kt++;
                    if (strncmp(kt, "if ", 3) == 0 || strcmp(kt, "if") == 0) { depth++; continue; }
                    if (strncmp(kt, "fi", 2) == 0 &&
                        (kt[2] == '\0' || kt[2] == ' ' || kt[2] == ';')) { depth--; continue; }
                    if (depth != 1) continue;
                    if (strncmp(kt, "elif ", 5) == 0) { next = k; break; }
                    if (strncmp(kt, "else", 4) == 0 &&
                        (kt[4] == '\0' || kt[4] == ' ' || kt[4] == ';')) {
                        next = k; next_is_else = 1; break;
                    }
                }

                if (!taken && eval_condition(cond)) {
                    rc = run_block(lines, cond_line + 1, next);
                    if (rc < 0) return rc;
                    taken = 1;
                }

                if (next == end) break;   /* no more branches */
                if (next_is_else) {
                    if (!taken)
                        rc = run_block(lines, next + 1, end);
                    break;
                }
                /* It was an `elif`: loop with cond_line=next. */
                cond_line = next;
                cond_kw   = "elif";
                kw_len    = 4;
                scan      = next + 1;
            }
            if (rc < 0) return rc;
            i = end + 1;
            continue;
        }

        if (strncmp(t, "while ", 6) == 0) {
            int end = find_match(lines, i + 1, to, "while", NULL, "done",
                                 NULL, 0, NULL);
            if (end < 0) { t_writestring("sh: missing done\n"); return -2; }
            static char cond[256];
            strncpy(cond, t + 6, sizeof(cond) - 1);
            cond[sizeof(cond) - 1] = '\0';
            char *dw = strstr(cond, "do");
            if (dw) {
                while (dw > cond && (dw[-1] == ' ' || dw[-1] == ';' || dw[-1] == '\t'))
                    dw--;
                *dw = '\0';
            }
            int guard = 0;
            while (eval_condition(cond)) {
                int rc = run_block(lines, i + 1, end);
                if (rc < 0) return rc;
                if (++guard > 100000) { t_writestring("sh: while-loop guard tripped\n"); break; }
            }
            i = end + 1;
            continue;
        }

        if (strncmp(t, "for ", 4) == 0) {
            int end = find_match(lines, i + 1, to, "for", NULL, "done",
                                 NULL, 0, NULL);
            if (end < 0) { t_writestring("sh: missing done\n"); return -2; }
            /* Parse: `for VAR in W1 W2 ...; do` */
            const char *p = t + 4;
            while (*p == ' ') p++;
            char var[64];
            size_t vi = 0;
            while (*p && is_ident_cont(*p) && vi + 1 < sizeof(var))
                var[vi++] = *p++;
            var[vi] = '\0';
            while (*p == ' ') p++;
            if (strncmp(p, "in", 2) != 0) {
                t_writestring("sh: bad for syntax\n"); return -2;
            }
            p += 2;
            static char wbuf[256];
            strncpy(wbuf, p, sizeof(wbuf) - 1);
            wbuf[sizeof(wbuf) - 1] = '\0';
            char *dw = strstr(wbuf, "do");
            if (dw) {
                while (dw > wbuf && (dw[-1] == ' ' || dw[-1] == ';' || dw[-1] == '\t'))
                    dw--;
                *dw = '\0';
            }
            static char expanded[512];
            if (sh_expand(wbuf, expanded, sizeof(expanded)) != 0) {
                t_writestring("sh: for-list expansion overflow\n");
                return -2;
            }
            char *argv[SHELL_MAX_ARGS];
            int argc = shell_parse(expanded, argv, SHELL_MAX_ARGS);
            for (int k = 0; k < argc; k++) {
                sh_vars_set(var, argv[k]);
                int rc = run_block(lines, i + 1, end);
                if (rc < 0) return rc;
            }
            i = end + 1;
            continue;
        }

        /* Plain command line.  Track its exit status in $? so scripts
         * can branch on it (bash-style). */
        static char copy[512];
        strncpy(copy, lines[i], sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        publish_status(sh_exec_line(copy) ? 0 : 127);
        i++;
    }
    return sh_last_status;
}

int sh_run_file(const char *path)
{
    /* Read into a scratch buffer.  256 KiB covers any realistic shell
     * script; if a file genuinely exceeds this, the layered fix is to
     * grow on demand from the heap (vfs has no stat() yet).  Until then
     * we warn on apparent truncation -- detected when the bytes read
     * exactly fill the buffer, which the kernel VFS treats as "more may
     * exist".  Detect, complain, keep going on the prefix. */
    enum { SH_MAX_SCRIPT = 262144 };
    char    *data = (char *)kmalloc(SH_MAX_SCRIPT);
    uint32_t sz = 0;
    if (!data) return -1;
    if (vfs_read_file(path, data, SH_MAX_SCRIPT - 1, &sz) != 0) {
        kfree(data);
        t_writestring("sh: cannot open ");
        t_writestring(path);
        t_writestring("\n");
        return -1;
    }
    data[sz] = '\0';
    if (sz == SH_MAX_SCRIPT - 1) {
        t_writestring("sh: warning: script may be truncated at ");
        /* Crude size print without depending on stdio. */
        t_writestring("256KiB; split into multiple files and chain via `sh`\n");
    }
    /* Build an array of line pointers by NUL-terminating each \n. */
    int nlines = 1;
    for (uint32_t i = 0; i < sz; i++) if (data[i] == '\n') nlines++;
    char **lines = (char **)kmalloc((size_t)nlines * sizeof(char *));
    if (!lines) { kfree(data); return -1; }
    int li = 0;
    lines[li++] = data;
    for (uint32_t i = 0; i < sz; i++) {
        if (data[i] == '\n') {
            data[i] = '\0';
            if ((uint32_t)(i + 1) < sz)
                lines[li++] = &data[i + 1];
        }
    }
    /* Pre-strip comments on every line so keyword recognition is clean. */
    for (int i = 0; i < li; i++) sh_strip_comment(lines[i]);
    int rc = run_block(lines, 0, li);
    kfree(lines);
    kfree(data);
    return rc;
}
