/*
 * shell_cmd_script.c - shell builtins for sh-style scripting.
 *
 * Exposes `sh`, `read`, `env`, `unset`, `export` (alias for assignment),
 * and the `[` test builtin so users can write conditional commands.
 * The heavy lifting (variable table, $VAR expansion, control flow) is
 * in sh_script.c.
 */

#include <kernel/sh_script.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>
#include <string.h>
#include "shell_priv.h"

static void cmd_sh(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: sh <script.sh>\n");
        return;
    }
    sh_run_file(argv[1]);
}

static void cmd_read(int argc, char **argv)
{
    if (argc < 2) {
        t_writestring("usage: read <VAR>\n");
        return;
    }
    char buf[256];
    shell_readline(buf, sizeof(buf));
    sh_vars_set(argv[1], buf);
}

static int env_print_cb(const char *name, const char *value, void *ctx)
{
    (void)ctx;
    t_writestring(name);
    t_writestring("=");
    t_writestring(value);
    t_writestring("\n");
    return 0;
}

static void cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    sh_vars_iter(env_print_cb, NULL);
}

static void cmd_unset(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        sh_vars_unset(argv[i]);
}

/* `[ ARGS ]` -- consumed via the dispatcher; result echoed.  Mostly
 * useful interactively; scripts use it inside if/while which are handled
 * by sh_run_file's interpreter directly. */
static void cmd_lbracket(int argc, char **argv)
{
    /* The closing `]` is just argv[argc-1]; drop it.  Failure to provide
     * it isn't fatal -- we still evaluate. */
    if (argc > 1 && strcmp(argv[argc - 1], "]") == 0) argc--;
    extern int sh_test_public(int argc, char **argv);
    /* Inline the test logic via sh_script.c's helper -- exposed
     * through a tiny wrapper to avoid plumbing.  For now, do the
     * simple inline. */
    int rc = 1;
    if (argc == 2) {
        rc = (*argv[1]) ? 0 : 1;
    } else if (argc == 3) {
        if (strcmp(argv[1], "-z") == 0) rc = (!*argv[2]) ? 0 : 1;
        else if (strcmp(argv[1], "-n") == 0) rc = ( *argv[2]) ? 0 : 1;
    } else if (argc == 4) {
        if (strcmp(argv[2], "=")  == 0) rc = strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        else if (strcmp(argv[2], "!=") == 0) rc = strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
    }
    /* Stash result in $? so interactive use is observable. */
    char rs[4];
    rs[0] = (char)('0' + rc); rs[1] = '\0';
    sh_vars_set("?", rs);
}

const shell_cmd_entry_t script_cmds[] = {
    { "sh",     cmd_sh,       0 },
    { "read",   cmd_read,     0 },
    { "env",    cmd_env,      0 },
    { "unset",  cmd_unset,    0 },
    { "[",      cmd_lbracket, 0 },
    { NULL,     NULL,         0 },
};
