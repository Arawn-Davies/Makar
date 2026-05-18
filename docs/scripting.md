---
title: Shell scripting
nav_order: 7
---

# Shell scripting

The Makar shell exposes a small bash-flavoured scripting layer.  No `fork()`, no pipes, no command substitution yet — everything runs inside the calling shell task.  Per-VT isolation: each shell's variable table hangs off its `task_t`, so `NAME=foo` on VT0 doesn't show up in VT1.

Implementation: `kernel/sh_script.h`, `arch/i386/shell/sh_script.c`, `arch/i386/shell/shell_cmd_script.c`.

## Variables

```sh
NAME=value           # per-task, no spaces around =, RHS shell-expanded
echo $NAME           # $VAR
echo ${NAME}_suffix  # ${VAR} for boundary cases
echo $?              # last command's exit status (0 on success, 127 unknown)
env                  # dump the table
unset NAME [...]     # remove vars
```

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
sh /cdrom/apps/demo.sh
./script.sh               # path ending in .sh goes through the interpreter
/cdrom/apps/script.sh     # absolute path, same dispatch
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

## Limitations (and what they're waiting on)

- **No command substitution (`$(cmd)`)** — would require capturing a child's stdout into a buffer; needs subshell-equivalent.
- **No pipes** — needs `fork()`.
- **No background jobs (`&`)** — needs `fork()`.
- **`elif` chained but `elif` itself can't appear on the same line as preceding body** — `; elif` is fine; `then A; elif [ Y ]; then B` works; bare `; elif` without preceding `; then BODY` may not parse.
- **64 KiB scratch buffer for script source** — warns on truncation; chain `sh foo.sh; sh bar.sh` for bigger workloads.

## Worked example

`src/userspace/demo.sh` is bundled into `/cdrom/apps/demo.sh` and `/hd/apps/demo.sh`.  It exercises every feature listed above with section markers (`cwd-ok`, `gt-ok`, `elif-correct-blue`, etc.) so a regression in any layer fails loudly.  Run with:

```sh
sh /cdrom/apps/demo.sh
```

or the bash-style equivalent:

```sh
/cdrom/apps/demo.sh
```
