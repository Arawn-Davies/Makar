---
title: Testing
parent: Getting started
nav_order: 2
---

# Testing Makar

Makar's tests are designed to run deterministically under QEMU and report
through serial markers. The old host-driven keyboard harness has been removed:
there is no QEMU monitor `sendkey` suite, no framebuffer screenshot diffing,
and no `tests/ui_test.sh`.

There are now two kinds of coverage:

1. **In-guest script and binary tests** for most behavior.
2. **In-guest keyboard injection** for behavior where the keystroke path itself
   is under test.

## Quick Commands

```sh
./run.sh ktest           # headless kernel test suite
./run.sh kbtest          # headless keyboard/makmux test
./run.sh kbtest gui      # same test with a visible QEMU window
./run.sh gui all-tests   # visible ktest + incore + libc + shell smoke
./run.sh iso test        # CI-style ISO gate
```

`./run.sh` with no arguments prints the full usage.

## Test Target Matrix

| Command | Display | Guest mode | Pass marker |
|---|---|---|---|
| `./run.sh ktest` | headless | `test_mode test=ktest` | `KTEST_RESULT: PASS` |
| `./run.sh guismoke` | headless | `test_mode test=gui-smoke` | `GUI-SMOKE: ALL PASS` |
| `./run.sh kbtest` | headless | normal boot plus `kbtest` cmdline | `KBTEST: ALL PASS` |
| `./run.sh kbtest gui` | visible | same as `kbtest` | `KBTEST: ALL PASS` |
| `./run.sh gui ktest` | visible | `test_mode test=ktest` | `KTEST_RESULT: PASS` |
| `./run.sh gui incore` | visible | `test_mode test=incore` | `INCORE: ALL PASS` |
| `./run.sh gui libc` | visible | `test_mode test=libc-tcc` | `LIBC-TCC: ALL PASS` |
| `./run.sh gui smoke` | visible | `test_mode test=shell-smoke` | `SHELL-SMOKE: ALL PASS` |
| `./run.sh gui all-tests` | visible | `test_mode test=all` | all markers |
| `./run.sh iso test` | headless | build + ktest + GDB checkpoint | command exit 0 |

## ktest

`ktest` is the kernel's internal unit/integration suite. It runs during
`test_mode` before the normal login shell.

Covered areas include:

- ACPI checksum validation
- x87 FPU bring-up and context-switch save/restore
- string helpers
- partition type helpers
- devfs
- tmpfs
- `/usr` sysroot layout
- rootfs mount layout
- physical memory manager
- kernel heap
- virtual memory and COW behavior
- task scheduler basics
- procfs task listing
- pid syscalls
- POSIX filesystem syscall aliases
- RTC/time syscalls
- syscall dispatcher basics
- fd tables
- growable file fds
- cwd isolation
- signals
- preemption
- GDT/IDT/ring-3 prerequisites
- ring-3 execution
- VESA mode and color behavior
- keyboard decoder layers

Run it headless:

```sh
./run.sh ktest
```

or visibly:

```sh
./run.sh gui ktest
```

## In-Guest Script Drivers

The script drivers live in `src/userspace/` and run inside the guest shell.
They avoid host typing entirely and gate on exit status plus serial markers.

### `incore.sh`

Runs core userspace binaries and checks their exit status:

- `hello`
- `forktest`
- `execvetest`
- `sigtest`
- `alloctest`
- `ktest_uspace`

Pass marker:

```text
INCORE: ALL PASS
```

### `libc-tcc.sh`

Runs the in-OS TCC/libc compile matrix. It compiles and, where relevant, runs
programs such as:

- `hello`
- `calc`
- `sh`
- `makbox`
- `makmux`
- `help`
- `diskinfo`
- `sigtest`
- `forktest`
- `execvetest`
- `alloctest`
- `filetest`
- `basic`
- `fdisk`
- `cfdisk`
- `maktop`
- `clock`
- `lines`
- `vix`
- `kbtester`

Pass marker:

```text
LIBC-TCC: ALL PASS
```

### `shell-smoke.sh`

Covers shell, VFS, and app behavior without host keyboard automation. It checks
commands and branches on `$?`, including:

- procfs and devfs visibility
- `exec /apps/hello.elf`
- `cd`
- mount listing
- `makbox`
- script variables
- `/tmp` file roundtrip
- `/usr` sysroot visibility
- nested parser state
- inline `if` branches
- `tty`
- quote grouping
- `/apps/sh.elf -c`
- ring-3 shell `if`, `for`, and quoting

Pass marker:

```text
SHELL-SMOKE: ALL PASS
```

### `gui-smoke.sh`

The headless GUI/desktop self-tests, kept as their own logical section (separate
from `shell-smoke.sh` and the kernel ktests) so they can run as a standalone,
fast, deterministic CI job. Each is a framebuffer-free `gui.elf` sub-command that
asserts in-process and exits non-zero on failure:

- `gui.elf uitest` — the `gui_ui` immediate-mode widgets (button/slider/textbox)
  against a synthetic off-screen surface.
- `gui.elf fstest` — the `gui_browser` file model against the real VFS
  (chdir/readdir/getcwd) — the Files window + the editor open/save dialog.
- `gui.elf desktest` — the desktop logic: the Windows-style column-major
  height-derived icon grid (never clipped off the bottom), on-screen clamping,
  the case-folded alphabetical sort, and the `~/.mxrc` icon-position key + value
  parse + a save→load round-trip.

Run only this suite with `./run.sh guismoke` (boots `test_mode test=gui-smoke`);
it is also part of a bare `test_mode` (so `./run.sh iso test` runs it too). Pass
marker:

```text
GUI-SMOKE: ALL PASS
```

It is deliberately isolated from the exec-heavy `libc-tcc.sh` / `shell-smoke.sh`
drivers so the CI `gui-headless` job stays non-flaky.

## kbtest

`kbtest` is the dedicated keyboard-under-test suite. It boots the normal shell
with `kbtest` on the kernel command line. The kernel spawns
`keyboard_test_driver()`, which injects keycodes directly into the live
keyboard path:

```text
keyboard_inject_text / keyboard_inject_key
    -> decoder path
    -> focus router
    -> task input ring
    -> live shell
```

This is the correct test for anything involving:

- PS/2 decode behavior
- modifiers
- Ctrl-C
- Alt-Tab / VT switching
- focused task routing
- `makmux`
- app tabs
- shell readiness under real input

Current scenarios:

1. Enable verbose serial mirroring.
2. Type `echo KBINJECT_OK`.
3. Start `cat`, send Ctrl-C, and prove the shell recovers with
   `KBTEST_CTRLC_OK`.
4. Type `makmux`.
5. Check `vtty_count()`.
6. Inject Alt-Tab and Alt-Shift-Tab and assert `vtty_active()` changes and
   restores.
7. Type `maktop`.
8. Assert `vtty_find_name("maktop") >= 0`, proving app-tab routing through
   makmux.
9. Quit `maktop`.
10. Type `exit` across the VT shells.
11. Assert `vtty_count() == 0`, proving makmux exited and focus restored.

Headless:

```sh
./run.sh kbtest
```

Visible:

```sh
./run.sh kbtest gui
```

## `iso test`

`./run.sh iso test` is the CI-style ISO gate. It performs:

1. incremental ISO build with test image enabled
2. headless ktest run
3. GDB ISO boot checkpoint run

Keyboard-under-test coverage is intentionally separate as `./run.sh kbtest`
because boot-to-shell under shared CI TCG can be slow.

## Logs

| Log | Produced by | Notes |
|---|---|---|
| `ktest.log` | `ktest`, `iso test`, visible test suites | serial transcript for ktest/script drivers |
| `kbtest.log` | `kbtest` | serial transcript for keyboard injection |
| `gdb-iso.log` | GDB ISO checkpoint | host-side GDB output |
| `*.log` | various targets | ignored by git |

When investigating a failure:

1. Search for the final suite marker.
2. Search for `FAIL`, `PAGE FAULT`, and `panic`.
3. Inspect the 50-100 lines before the first failing marker.
4. For keyboard/makmux failures, use `kbtest.log`, not `ktest.log`.

## Adding Tests

Choose the lightest test path that covers the behavior:

| Behavior | Preferred test |
|---|---|
| pure kernel helper | `ktest` |
| syscall behavior directly | `ktest` or a small userspace app |
| userspace app exit behavior | `incore.sh` |
| libc wrapper behavior | `alloctest.elf` or `libc-tcc.sh` |
| shell syntax/dispatch | `shell-smoke.sh` |
| keyboard focus/input/VT behavior | `keyboard_test_driver()` / `kbtest` |

Avoid reintroducing host-driven typing. If input must be tested, inject it from
inside the guest.
