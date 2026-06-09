---
title: Shell scripting
parent: Using Makar
nav_order: 2
---

# Shell scripting

Makar currently has two sh-flavoured command layers:

| Layer | Location | Main role |
|---|---|---|
| Userspace shell | `/apps/sh.elf`, source `src/userspace/sh.c` | The default interactive shell on ring-3 VTs. Supports PATH lookup, quoting, `sh -c`, pipes, redirection, `&&`, `||`, background `&`, and `wait`. |
| Kernel script runner | `arch/i386/shell/sh_script.c` | A compact in-kernel interpreter used by `sh /path/script.sh` and boot/test scripts such as `shell-smoke.sh`, `libc-tcc.sh`, and `incore.sh`. |

This page documents the in-kernel script runner. The userspace shell is more
POSIX-shaped and is what operators normally interact with. The kernel runner is
kept deliberately smaller because it dispatches commands inside the calling
shell task rather than forking a full subshell for every command.

`fork()`, `execve()`, and `wait4()` are available kernel-wide, and `/apps/sh.elf`
uses them. The in-kernel script layer only uses that machinery when a command it
dispatches chooses to launch a userspace executable.

Per-VT isolation: each shell's variable table hangs off its `task_t`, so
`NAME=foo` on one VT does not appear in another VT.

Implementation:

- `src/kernel/include/kernel/sh_script.h`
- `src/kernel/arch/i386/shell/sh_script.c`
- `src/kernel/arch/i386/shell/shell_cmd_script.c`

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

Variable expansion is intentionally simple:

- `$NAME` expands until the first non-name character;
- `${NAME}` is available when a suffix immediately follows;
- unknown variables expand to an empty string;
- `$?` expands to the last command status.

The expansion happens before command dispatch. There is no command
substitution, arithmetic expansion, array syntax, or environment export model
in the kernel script runner.

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

The parser is line-oriented but can keep enough block state to handle
multi-line `if`, `while`, and `for` forms. It is designed for deterministic
boot/test scripts, not for arbitrary POSIX shell compatibility.

## Running scripts

```sh
sh /apps/demo.sh
./script.sh               # path ending in .sh goes through the interpreter
/apps/script.sh           # absolute path, same dispatch
sh -c 'echo hello'         # supported by userspace sh.elf, not this runner
```

The `sh` builtin writes the script's final `$?` to serial as `sh: exit=N` so test runners can scrape the value.

## Builtins relevant to scripting

| Command | Notes |
|---|---|
| `sh PATH` | Run a script file (path on the VFS). |
| `read VAR` | One line of input → `VAR`. |
| `env` / `unset` | Variable table dump / remove. |
| `[ ... ]` | Test (also usable interactively). |
| `sleep N` | Busy-yield N seconds (250 Hz PIT). |
| `true` / `false` | POSIX status helpers. |
| `datetime` / `date` / `time` | One-line `YYYY-MM-DD HH:MM:SS` from `/proc/rtc`.  For the fullscreen wall clock, use `clock.elf`. |

## Userspace shell features

Use `/apps/sh.elf` when you need the fuller shell surface:

| Feature | Userspace `sh.elf` | Kernel script runner |
|---|---|---|
| quote-aware tokenization | yes | limited |
| `sh -c COMMAND` | yes | no |
| pipes (`|`) | yes | no |
| redirection (`<`, `>`, `>>`, `2>`, `2>>`) | yes | no |
| list operators (`&&`, `||`) | yes | no |
| background jobs (`&`) | yes | no |
| `wait` builtin | yes | no |
| in-process boot/test dispatch | no | yes |

The userspace shell relies on the kernel's `pipe`, `dup`, `dup2`, `fork`,
`execve`, and `wait4` syscalls. The kernel runner has not been rewired to use
those operators because its main value is deterministic in-process execution
during boot and test modes.

## Limitations of the kernel script runner

- **No command substitution (`$(cmd)`).** Capturing command output would require
  a pipe-backed subshell model.
- **No pipe/redirection/list/background syntax.** Use `/apps/sh.elf` for that.
- **No exported environment.** Variables are per-shell task state.
- **No `wait` builtin or background jobs.** Backgrounding has no meaning while
  dispatch remains in-process.
- **64 KiB scratch buffer for script source.** The runner warns on truncation;
  chain smaller scripts when needed.

## Worked example

`src/userspace/demo.sh` is bundled into `/apps/demo.sh` (rootfs election routes to whichever volume is the active boot medium).  It exercises every feature listed above with section markers (`cwd-ok`, `gt-ok`, `elif-correct-blue`, etc.) so a regression in any layer fails loudly.  Run with:

```sh
sh /apps/demo.sh
```

or the bash-style equivalent:

```sh
/apps/demo.sh
```
