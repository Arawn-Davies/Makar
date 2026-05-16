---
title: History
nav_order: 7
---

# History

A diary of how Makar got here, drawn from the git log and PR archive.
Dates are commit/merge dates; PR numbers link to the merge that
shipped each milestone.

## 2019 — First strokes

**August 2019.** First commit, display driver newline fix, libc
stubs. By the end of the week: IDT, GDT, basic IRQ handling, a serial
debug interface. The project was unnamed and the build was driven by
a hand-rolled QEMU shell script.

> Then the project sat dormant for almost five years.

## 2020 — A brief return

**August 2020.** Editorconfig, code-style consolidation, a switch
from GAS-style inline assembly to standalone NASM files. Nothing
shipped beyond the existing scaffold. Repo went quiet again after
about a week.

## 2025 — Single-commit hiatus

**January 2025.** One commit: "fix assembly comments." A drive-by
fix while the rest of the codebase still slept.

## 2026-04 — Rebirth

**April 9–10.** Project rename to "Untitled OS." GitHub Actions wired
up, push triggers, a Copilot-assisted PR cadence begins (#1–#17). The
build acquires real CI, the kernel acquires real structure.

**April 11.** Repo reshuffle: source tree moves to `src/`, kernel
under `src/kernel/arch/i386/`. Cooperative multitasking and an `int
0x80` syscall stub land (#42). First ATA PIO IDE driver (#41).

**April 12.** Memory work: paging cleanup, ACPI, on-demand ktest
harness (#44). ATA + MBR + GPT partitions, `mkpart` shell command
(#45). The Makar name takes shape — shell vocabulary deliberately
mirrors Medli (#48). Build-timestamp banner at boot, shell split into
focused source files (#46). FAT32 + Medli filesystem layout (#26 /
#47). Universal VFS, ↑/↓ shell history, installer (#49). Docker
image swapped for the i686-elf cross-compiler.

**April 25.** VMM lands. Ring-3 trampoline, comprehensive ktest
suites (#53). Cross-platform build fix for macOS arm64.

**April 26–27.** Panic screen + startup logo (#110). Display fixes +
VESA ktest (#111). Readline inline editing, file-I/O commands, ring-3
stdin, `exec` wait (#112).

**April 29–30.** VESA pane abstraction (#113, "split-panes phase 1").
Installed-HDD boot path: image generation, interactive boot, GDB test
(#114). Auto-release workflow on main merge (#115). `run.sh`
consolidation — one entrypoint for build, test, boot (#116).

## 2026-05 — The big runway

**May 1.** GRUB menu + 720p default + sigint + tab completion + calc
+ ktest improvements (#117). Linux i386 ABI userspace with `exec`
shell command (#118).

**May 2.** Multi-TTY shell, pane-aware VICS editor, `lsman` + `man`
pages (#119). FAT32 userspace fileops — delete, rename, syscalls
208–210, ELF apps `rm.elf` / `mv.elf` / `cp.elf` (#120).

**May 8.** Preemptive 100 Hz scheduler. Per-task `task_t`
plumbing (`pid`, `cwd`, `tty`, signal bitmasks, fd-table placeholder),
ring-3 lifecycle proof via ktest, user-PD reaper (#123).

**May 12.** Layered PS/2 keyboard rewrite: scancode → keycode →
sentinel → router pipeline, IRQ-driven per-task SPSC rings,
`kbtester.elf` ring-3 diagnostic (#124).

**May 13.** Build/CI overhaul: ccache, single-kernel/two-ISO emit,
build-once fan-out CI with 4 parallel jobs, KVM auto-detect (off by
default), local `act` validation (#125).

**May 14.** Per-TTY backing buffers (`vt_buf_t`), deferred FB repaint
on Alt+Fn, tmux-style status bar at the bottom row, synthetic
`/proc` filesystem with `cpuinfo`/`meminfo`/`tasks`/`uname`, glob +
tab completion across the VFS, `MAKAR_VERSION` single-source, tagged
v0.5.0 (#129).

**May 15.** VIX rename (was VICS — "vi C-Sharp" no longer applied,
language-neutral now). vim-style line-number gutter, word wrap,
flashing block caret. Cross-FS root enumeration in `vfs_complete`
(`cd /<TAB>` finally works). Linux-style serial console
(`g_serial_verbose`, `console=ttyS0` cmdline, `verbose` shell
builtin). UI-test framework via QEMU HMP `sendkey` (3 scenarios).
Shell-side framebuffer restore after fullscreen commands. Per-suite
ktest descriptions (#130).

## Next

Tracked in [CLAUDE.md "Slice queue"](https://github.com/Arawn-Davies/Makar/blob/main/CLAUDE.md#slice-queue-feat-tty-multitasking--follow-ups)
on the live repo. Headline:

- **Slice 14 (NEXT)** — Per-task FD table. Replaces the global
  keyboard owner + placeholder `fd_table` with a real per-task
  array. Prerequisite for any libc port.
- **Slice 8** — Linux-style signal subsystem (sigaction, `kill()`).
- **Slice 9** — Preemption hardening (interrupt-safe `schedule()`).
- **Slice 15** — VFS `task->cwd` authoritative.
- **Slice 16** — VGA-text fallback per-TTY backing buffers.

Long term: musl libc port → dash → in-kernel TCC for write-compile-
run on bare metal. See [Userland libc](userland-libc.md).
