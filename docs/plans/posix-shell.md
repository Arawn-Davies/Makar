# Plan: POSIX-compliant shell features for /apps/sh.elf

## Context

After the userspace-shell migration lands, `/apps/sh.elf` will be the
default interactive shell on every VT.  It already covers the in-kernel
shell's surface (vars, scripting, tab cycling, glob, admin syscalls),
but it's still a long way from a bash/zsh-class shell.  The Unix-y
features below are deliberately out of scope for the migration PR and
should be picked up as their own slices once that lands.

This is a feature ladder, not an architectural plan -- each item is
mostly independent and can be picked up in any order.

## Wanted features

### Plumbing
- **Pipes (`cmd1 | cmd2`)**:
  Requires `SYS_PIPE` (returns two fds), `SYS_DUP2`, and a shell
  parser change to split on `|`, fork for each stage, and wire up
  dup2(pipefd[0/1], STDIN/STDOUT_FILENO) before exec.  Kernel-side
  this needs an in-memory pipe ring (4 KiB ring per pipe, blocking
  read on empty, blocking write on full).
- **Redirection (`< > >> 2> &>`)**:
  Parser change to recognise the operators and consume the filename
  token.  In the child after fork, open() the file and dup2() onto
  stdin/stdout/stderr.  `2>` and `&>` need the parser to track which
  fd to redirect.
- **Background (`&`)**:
  Don't `sys_wait4` after the fork; record the pid in a job table and
  print `[1] <pid>` style.  Needs `wait` builtin and `jobs` listing.
  Probably wants `SIGCHLD` delivery so the shell can reap zombies.
- **Command substitution (`$(cmd)`, backticks)**:
  Fork+pipe; read the child's stdout into a buffer; substitute into
  the parent's argv pre-tokenize.  Composes with pipes.

### Quoting / parsing
- Double-quoted strings (`"foo $bar baz"`) with `$` expansion inside.
- Single quotes (`'literal $bar'`) -- no expansion.
- Backslash escapes (`\$`, `\"`, etc.).
- Inline `NAME=VAL CMD args` env-prefix assignments (POSIX env(1)
  shorthand).  Today only standalone `NAME=VAL` is supported.

### Control / flow
- Logical AND/OR (`cmd1 && cmd2`, `cmd1 || cmd2`).
- `case ... esac` for pattern matching.
- `function name() { ... }` definitions (or `name() { ... }`).
- `break N` / `continue N` for nested loops.
- `return` from sourced scripts and functions.
- Subshells (`( ... )`) -- fork, run, parent waits.

### Job control
- `jobs`, `fg`, `bg` builtins.
- TTY foregrounding -- requires `tcsetpgrp` equivalent + signals
  routed by pgid not pid.  Big lift; needs process-group support in
  `task_t`.

### Environment
- Real `environ` -- argv envp passthrough (SYS_EXECVE already takes
  envp; need shell to actually pass its env table through, and child
  processes to receive/parse it via `__libc_init`).
- `export NAME[=val]` -- mark a var to be passed in envp.
- `unset -e NAME`, `set -e`, `set -u`, `set -x` (`-x` is the most
  useful for debugging scripts).
- `$0..$9`, `$#`, `$@`, `$*` positional argument expansion.
- `$$` (pid of the shell), `$!` (last backgrounded pid).

### Builtins to add
- `alias` / `unalias`.
- `type` / `which` (look up where a command would come from).
- `getopts` for script argument parsing.
- `local` (function-scoped variables; needs functions first).
- `printf` (POSIX printf builtin, not just echo).

### Polish
- `~` and `~user` expansion (HOME, /home/<user>).
- `cd -` (return to previous directory).
- Brace expansion (`{a,b,c}` / `{1..10}`).
- History expansion (`!N`, `!!` -- in-kernel shell has `!!`; userspace
  should match).
- Tab completion polish: complete env vars (`$VA<Tab>`), command names
  from PATH (currently only path components).

### Underlying syscalls needed (rough list)
| Syscall | Purpose |
|---|---|
| SYS_PIPE     | pipe(2) for `|` |
| SYS_DUP / SYS_DUP2 | redirection + pipe wiring |
| SYS_WAITPID / SYS_WAIT | richer than wait4 for job-control reaping |
| SYS_GETPGID / SYS_SETPGID | process groups for job control |
| SYS_TCSETPGRP | foreground a job; requires VT-pgrp model |
| SYS_SIGACTION | richer signal handling than SYS_SIGNAL |
| SYS_GETUID / SYS_SETUID | once a user model exists |

## Suggested order

1. Pipes + dup2 (unlocks most Unix idioms).
2. Redirection (cheap once dup2 lands).
3. Logical && and ||, plus background `&` (small parser changes).
4. Command substitution `$(cmd)` (needs pipes).
5. Quoting (double/single) -- big tokenizer rewrite, ship as one slice.
6. Functions + positional args.
7. Job control (requires process groups; biggest lift).

## Out of scope (forever, probably)

- ksh array syntax (`a[0]=v`, `${a[@]}`).
- Process substitution (`<(cmd)`, `>(cmd)`) -- needs /dev/fd.
- bash-specific extensions (BASH_REMATCH, regex `[[ =~ ]]`).
- `select` interactive menus.

Keep `sh.elf` POSIX 2017 shell-compliant, not bash-compatible.
