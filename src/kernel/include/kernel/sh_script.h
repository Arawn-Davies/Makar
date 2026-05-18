/*
 * sh_script.h - bash-flavoured scripting for the Makar shell.
 *
 * Surface area kept deliberately small: per-shell-task variable table,
 * $VAR expansion, NAME=value assignment, `read VAR`, `sh script.sh`,
 * `#` comments, and a line-oriented if/while/for parser.
 *
 * Per-shell-task isolation: the variable table hangs off task_t.script_vars
 * so each VT's shell owns its own environment.  Matches the per-VT-palette
 * env-var model -- variables don't leak across VTs.
 */
#ifndef MAKAR_SH_SCRIPT_H
#define MAKAR_SH_SCRIPT_H

#include <stddef.h>

/* --- variable table ------------------------------------------------------- */

/* Fetch the value of NAME for the calling task.  Returns NULL if unset.
 * Pointer is valid until the next sh_vars_set / sh_vars_clear call. */
const char *sh_vars_get(const char *name);

/* Set NAME=value for the calling task.  Creates the table on first call;
 * grows it as needed.  Returns 0 on success, -1 on OOM. */
int sh_vars_set(const char *name, const char *value);

/* Unset NAME.  No-op if not present. */
void sh_vars_unset(const char *name);

/* Iterate: callback receives each name/value pair until it returns non-zero.
 * Used by `env` / debugging dumps. */
typedef int (*sh_vars_iter_cb)(const char *name, const char *value, void *ctx);
void sh_vars_iter(sh_vars_iter_cb cb, void *ctx);

/* Free the calling task's table.  Called by task_exit cleanup. */
void sh_vars_free_for(void *task);

/* --- line-level execution ------------------------------------------------- */

/* Expand $VAR / ${VAR} in `in`, write result to `out` (up to outsz-1).
 * Unset variables expand to empty string.  Returns 0 on success, -1 if
 * output would overflow. */
int sh_expand(const char *in, char *out, size_t outsz);

/* Detect and apply `NAME=value` assignment at the start of `line`.
 * Returns 1 if line was an assignment (and applied it), 0 otherwise.
 * value is shell-expanded before storage. */
int sh_try_assign(const char *line);

/* Strip trailing # comment (outside single/double quotes) from line. */
void sh_strip_comment(char *line);

/* Execute a single already-expanded, comment-stripped line through the
 * shell dispatcher.  Empty / whitespace-only lines are no-ops.
 * Returns the dispatcher's return code (1=found, 0=not found). */
int sh_exec_line(char *line);

/* --- script execution ----------------------------------------------------- */

/* Run a script file from the VFS.  Reads, splits into lines, walks with
 * the if/while/for interpreter.  Returns 0 on success, -1 on I/O error,
 * -2 on parse error. */
int sh_run_file(const char *path);

#endif /* MAKAR_SH_SCRIPT_H */
