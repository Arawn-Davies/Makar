---
title: Shell scripting
parent: Using Makar
nav_order: 2
---

# Shell scripting

There are now **two** sh-flavoured interpreters in Makar.  This page documents the **in-kernel** scripting layer (`sh_script.c`) used by `sh /path/script.sh` and the test bootstraps (`shell-smoke.sh`, `libc-tcc.sh`, `incore.sh`).  The **userspace** shell (`/apps/sh.elf`, source `src/userspace/sh.c`) is what the operator types into interactively on every VT, and it ships with extras the in-kernel script layer doesn't have — pipes (`\|`), redirection (`< > >> 2> 2>>`), and list operators (`&& \|\| &` + `wait`).  Those landed in PR #181 and live in userspace only.

`fork()` / `execve()` / `wait4()` are available kernel-wide.  The in-kernel script layer dispatches via the same `shell_dispatch_argv` path the interactive prompt uses; everything below still runs inside the calling shell task (no subshell).  Per-VT isolation: each shell's variable table hangs off its `task_t`, so `NAME=foo` on VT0 doesn't show up in VT1.

Implementation: `kernel/sh_script.h`, `arch/i386/shell/sh_script.c`, `arch/i386/shell/shell_cmd_script.c`.

## Variables

```sh
NAME=value           # per-task, no spaces around =, RHS shell-expanded
echo $NAME           # $VAR
echo ${NAME}_suffix  # ${VAR} for boundary cases
echo $?              # last command's exit status (0 on success, 127 unknown)
                     # For `exec <elf>` lines, $? reflects the child's
                     # SYS_EXIT value (low 8 bits) via shell_last_exec_status();
                     # builtins yield $?=0 (no failure-status threading yet).
env                  # dump the table
unset NAME [...]     # remove vars
```

`PATH` is a special variable: command dispatch and first-token tab completion
search its colon-separated directories for `<cmd>[.elf]`. Unset, it defaults to
`/apps`; set it like any other var to change where the
shell looks for executables.

## Tests

```sh
[ STR ]              # non-empty string
[ -z STR ]           # empty
[ -n STR ]           # non-empty (explicit)
[ STR1 = STR2 ]      # string equal
[ STR1 != STR2 ]     # string not equal

[ N1 -eq N2 ]        # integer equal       (-ne, -lt, -le, -gt, -ge)
                     # non-numeric operand: error, $? = 2
```

## Control flow

```sh
if [ TEST ]; then
    ...
elif [ TEST ]; then
    ...
else
    ...
fi

# Single-line form
if [ TEST ]; then CMD; fi

while [ TEST ]; do
    ...
done

for VAR in WORD1 WORD2 WORD3; do
    ...
done
```

Multi-statement lines split on `;` are supported (`if [ X ]; then A; elif [ Y ]; then B; else C; fi` works on a single line).  `# ...` comments terminate the line outside quotes.

## Running scripts

```sh
sh /apps/demo.sh
./script.sh               # path ending in .sh goes through the interpreter
/apps/script.sh           # absolute path, same dispatch
```

The `sh` builtin writes the script's final `$?` to serial as `sh: exit=N` so test runners can scrape the value.

## Builtins relevant to scripting

| Command | Notes |
|---|---|
| `sh PATH` | Run a script file (path on the VFS). |
| `read VAR` | One line of input → `VAR`. |
| `env` / `unset` | Variable table dump / remove. |
| `[ ... ]` | Test (also usable interactively). |
| `sleep N` | Busy-yield N seconds (100 Hz PIT). |
| `true` / `false` | POSIX status helpers. |
| `datetime` / `date` / `time` | One-line `YYYY-MM-DD HH:MM:SS` from `/proc/rtc`.  For the fullscreen wall clock, use `clock.elf`. |

## Limitations of this layer (and where to find the missing features)

- **No command substitution (`$(cmd)`)** — would require capturing a child's stdout into a buffer; needs subshell-equivalent.
- **No pipes (`\|`), redirection (`< > >> 2> 2>>`), or list operators (`&& \|\| &`) here** — these all exist in `/apps/sh.elf` since PR #181 (kernel-side `SYS_PIPE`/`SYS_DUP2`/`FD_KIND_PIPE` ship in the same PR).  The in-kernel script interpreter (`sh_script.c`) hasn't been rewired to use them; if you need them, drive your workload through `sh.elf` instead.
- **No `wait` builtin / `&` background here** — present in `sh.elf`; deferred from `sh_script.c` because the in-kernel script layer would need to fork-then-dispatch first (currently it dispatches in-process so backgrounding has no meaning).
- **`elif` chained but `elif` itself can't appear on the same line as preceding body** — `; elif` is fine; `then A; elif [ Y ]; then B` works; bare `; elif` without preceding `; then BODY` may not parse.
- **64 KiB scratch buffer for script source** — warns on truncation; chain `sh foo.sh; sh bar.sh` for bigger workloads.

## Worked example

`src/userspace/demo.sh` is bundled into `/apps/demo.sh` (rootfs election routes to whichever volume is the active boot medium).  It exercises every feature listed above with section markers (`cwd-ok`, `gt-ok`, `elif-correct-blue`, etc.) so a regression in any layer fails loudly.  Run with:

```sh
sh /apps/demo.sh
```

or the bash-style equivalent:

```sh
/apps/demo.sh
```
