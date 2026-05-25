# Plan: expand `tcc_rebuild_*` coverage across the userspace apps

## Context

In-OS TCC self-rebuild is already proven for `hello.elf`, `calc.elf`,
`sh.elf`, and `makbox.elf` via `tests/ui_test.sh:test_tcc_rebuild_*`.
After the recent kernel + libc.a fixes (`USER_STACK_PAGES = 8`,
`objcopy --strip-debug libc.a`), the remaining 16 standalone userspace
binaries *should* rebuild without further code changes -- but only
those four are actually verified.

This plan adds rebuild-and-run scenarios for the rest of `PROGS` (and
`alloctest`) so we have full coverage that in-OS TCC can rebuild every
ELF the OS ships.  Each new scenario follows the existing two-stage
send pattern (`test_tcc_rebuild_calc` in `tests/ui_test.sh:836` is the
canonical reference) -- no new framework primitives needed.

## What to add

For each app listed below, add a `test_tcc_rebuild_<name>` function in
`tests/ui_test.sh` that:

1. `reset_shell`
2. send `tcc /src/userspace/<name>.c -o /tmp/<name>-rebuilt.elf` + Enter
3. `wait_for_serial '\[shell:ready vt=0\]'` with a 90s budget (TCC is
   slow under TCG; basic.c / maktop.c may want 120s)
4. `it_until` the rebuilt binary with an assertion-specific marker

Also register each one in `LIBC_TESTS` (or a new `TCC_REBUILD_TESTS`
group) at the bottom of `ui_test.sh` so `./run.sh ui libc` picks them
up.

### Strong candidates — freestanding (`syscall.h` only), no libc.a link

| App | Suggested marker for `it_until` | Notes |
|---|---|---|
| `vix.elf` | requires interactive input -- assert on banner via `exec /tmp/vix-rebuilt.elf /tmp/x.txt` + immediate Ctrl-Q | fullscreen; needs the same screen-snapshot reset as the existing vix tests |
| `maktop.elf` | `Tasks: ` header | fullscreen, but writes line-mode banner first |
| `cfdisk.elf` | `cfdisk` banner | fullscreen MBR editor |
| `fdisk.elf` | `fdisk:` prompt | line-driven |
| `basic.elf` | `READY.` | type `10 PRINT 1+1` + `RUN` after launch |
| `kbtester.elf` | `kbtester: live diagnostic` | needs `keys "q"` to exit |
| `clock.elf` | RTC time string `HH:MM:SS DD/MM/YY` | fullscreen; Ctrl-C exit |
| `lines.elf` | `lines: done` | runs to completion |
| `forktest.elf` | `[forktest] PARENT-POST` | exits 0 |
| `execvetest.elf` | `[execve-test] POST-EXEC parent_pid_alive` | exits 0 |
| `sigtest.elf` | `[sigtest] handler ran` | exits 0 |
| `filetest.elf` | `[filetest] PASS` | needs a writable mountpoint at `/mnt/scratch` -- copy the pattern from existing `test_filetest` (it formats `/dev/hda` ext2 first) |
| `diskinfo.elf` | `Disk hda:` | runs to completion |
| `help.elf` | `usage:` banner | trivial |

### Libc-using

| App | Why valuable | Marker |
|---|---|---|
| `alloctest.elf` | end-to-end exercise of `libc.a` link path (the same archive that bit `sh.c` until the `objcopy --strip-debug` fix landed) | `[alloctest] PASS` |

## Reference pattern (copy this verbatim)

```sh
test_tcc_rebuild_<name>() {
    reset_shell
    CURRENT_NAME=tcc-rebuild-<name>
    local sb1=$(wc -c < "$SERIAL_LOG")
    send_script "$(keys "tcc /src/userspace/<name>.c -o /tmp/<name>-rebuilt.elf")
sendkey ret"
    wait_for_serial '\[shell:ready vt=0\]' "$sb1" 90 || \
        echo "  - stage1: tcc compile never returned to prompt"
    it_until "tcc-rebuild-<name>" \
"$(keys "exec /tmp/<name>-rebuilt.elf <ARGS>")
sendkey ret
PAUSE 0.8
<any follow-up keys + sendkey ret to drive the binary>" \
        "<marker that proves it ran>" 30
    assert_serial_contains "<marker>"
    assert_serial_not_contains "Kernel panic" "SIGSEGV"
}
```

Notes:

- The reaper-output keystroke race is already closed in the framework
  (`POST_RET_DELAY` in `tests/ui_runner.sh`).  Don't reintroduce
  per-scenario `PAUSE` workarounds.
- The two-stage send (`send_script` + `wait_for_serial` + `it_until`)
  is required because TCC compile takes ~5-10s under TCG -- queueing
  the exec keystrokes during that window loses the focus-transfer
  race.  All current `tcc_rebuild_*` scenarios do this.
- For fullscreen apps (vix, maktop, cfdisk, clock), expect a screen
  repaint after the child reaps -- the runner's existing snapshot
  capture handles this, no extra reset.

## Critical files

- `tests/ui_test.sh` -- add scenarios + register in test groups
- `tests/ui_runner.sh` -- read-only; reference `send_script`,
  `wait_for_serial`, `it_until`, `keys`, `assert_serial_contains`
- `src/userspace/*.c` -- read-only; source-of-truth for each app's
  startup banner / exit marker

## Verification

End-to-end:

```sh
./run.sh ui libc                        # all new scenarios pass
./run.sh ui tcc_rebuild_filetest        # one scenario in isolation
./run.sh ui                             # full suite still green
```

CI piggybacks on the existing UI suite; no workflow YAML change
needed.  The new scenarios will lengthen the local UI run (each TCC
compile is ~5-10s under TCG), but they're not in the `ktest` /
`incore` CI path which only covers tests that don't need HMP.

## POSIX compliance notes

All the apps in scope here are freestanding C (only `#include
"syscall.h"`) or use the Makar libc shim (`libc.a`).  Neither touches
the parts of POSIX Makar doesn't have -- no floats, no `mmap`, no
pipes / redirection, no threads, no real `errno` semantics, no
`termios` (terminal control goes through Makar exts like
`SYS_TERM_SIZE` / `SYS_PUTCH_AT` / `SYS_CARET_STYLE`).  See
[`docs/posix.md`](../posix.md) for the full surface map.

What this means for the new tests:

- **Don't** introduce new test apps that depend on POSIX features
  Makar lacks -- they won't compile under TCC even if they look
  plausible.
- **Do** mirror the success pattern of the existing rebuilds: each
  app prints a marker on success / failure to serial via
  `SYS_WRITE_SERIAL`, exits via `SYS_EXIT(0)` or `SYS_EXIT(1)`, and
  the `it_until` marker lives in serial.
- **Don't** rely on `$?` for builtin exit status (still always 0 for
  builtins -- only `exec <elf>` lines surface the child's status).
  The `incore.sh` driver pattern is `exec <elf>; if [ $? -eq 0 ] ...`
  which works because `exec` goes through `shell_last_exec_status()`.

## Out of scope

- `tcc_compat.c`, `malloc.c`, `stdio.c`, `string.c` -- these aren't
  standalone binaries; they're libc components archived into
  `libc.a`.
- Speed work on `wait_for_serial` -- the `filetest` / `alloctest`
  scenarios already burn their full timeout on every run despite
  passing; that's a separate investigation.
- Adding these tests to the CI `ktest` / `incore` path -- HMP-driven
  scenarios stay local-only per the policy in
  `.github/workflows/build-test.yml`.
