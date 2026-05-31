---
title: Shell follow-ups
parent: Plans
grand_parent: Development
nav_order: 2
---

# Shell follow-ups

`/apps/sh.elf` is already the default interactive shell. Pipes, redirection,
`&&`, `||`, background `&`, and `wait` shipped in PR #181. This page tracks
the remaining POSIX-shaped shell work.

## Parsing and expansion

- Double and single quotes with correct expansion rules.
- Backslash escapes.
- Inline `NAME=VALUE command` assignments.
- Command substitution with `$(command)`.
- Positional parameters: `$0` through `$9`, `$#`, `$@`, and `$*`.
- Shell PID and last background PID: `$$` and `$!`.

## Control flow

- `case ... esac`.
- Functions, `return`, and function-local variables.
- `break N` and `continue N`.
- Subshells with `( ... )`.

## Environment

- Pass a real `envp` through `execve`.
- Add `export`.
- Add shell options such as `set -e`, `set -u`, and `set -x`.

## Job control

- Add `jobs`, `fg`, and `bg`.
- Add process groups and a `tcsetpgrp` equivalent before claiming real
  foreground job control.
- Add richer signal handling where job control needs it.

## Smaller builtins and polish

- Add `alias`, `unalias`, `type`, `which`, `getopts`, and `printf`.
- Add `cd -` and tilde expansion.
- Complete command names from `PATH` and variables after `$`.

## Suggested order

1. Quote-aware tokenization and escapes.
2. Real `envp`, `export`, and inline assignments.
3. Positional parameters and functions.
4. Command substitution and subshells.
5. Process groups and interactive job control.
