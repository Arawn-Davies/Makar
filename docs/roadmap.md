---
title: Roadmap
parent: Development
nav_order: 1
---

# Roadmap

Where work stands today (May 2026) and what's queued up next.  Work
is tracked as two parallel streams: the **slice queue** below
(kernel/userspace work units, each one PR) and the GitHub issue
tracker (everything else).  This page is the map between them.

For the chronological log of what's *already shipped*, see
[history](history.md).

## Slice queue

Kernel and userspace work, numbered as focused mergeable units —
usually one PR per slice.

Slice numbers were resequenced in May 2026 so the queue runs 1..N
without gaps or duplicates.  CLAUDE.md's slice queue is the source of
truth; this table mirrors it.

| Slice | Title                                                  | Status                      | Issue                                                                              |
|------:|--------------------------------------------------------|-----------------------------|------------------------------------------------------------------------------------|
|     1 | Reaper for dead-task user PDs                          | ✅ shipped (`fcb8771`)      | —                                                                                  |
|     2 | Per-task `task_t` plumbing                             | ✅ shipped (`3a0ef78`)      | —                                                                                  |
|     3 | Ring-3 lifecycle ktest                                 | ✅ shipped (`f48d730`)      | —                                                                                  |
|     4 | 100 Hz timer + `SYS_WRITE_SERIAL`                      | ✅ shipped (`5e40001`)      | —                                                                                  |
|     5 | Keyboard rewrite (layered decoder)                     | ✅ shipped (#124)           | —                                                                                  |
|     6 | Keyboard hardening                                     | ✅ shipped (#127)           | —                                                                                  |
|     7 | Test-infra cleanup (ccache, fan-out CI)                | ✅ shipped (#125)           | —                                                                                  |
|     8 | Per-task consumer migration (vtty/fd/cwd)              | ✅ complete                 | —                                                                                  |
|     9 | Linux-style signal subsystem                           | ✅ shipped (#154)           | [#144](https://github.com/Arawn-Davies/makar/issues/144)                          |
|    10 | Preemption hardening                                   | ✅ shipped (#154)           | [#145](https://github.com/Arawn-Davies/makar/issues/145)                          |
|    11 | Per-TTY screen buffers + status bar + `/proc`          | ✅ shipped (#129)           | —                                                                                  |
|    12 | Per-task FD table                                      | ✅ shipped (#134)           | —                                                                                  |
|    13 | VFS `task->cwd` authoritative                          | ✅ shipped (#135)           | —                                                                                  |
|    14 | makbox multicall + `SYS_GETCWD` + exec race fix        | ✅ shipped (#137)           | —                                                                                  |
|    15 | fork() + copy-on-write (15a-e)                         | ✅ shipped (#166)           | rolled into [#121](https://github.com/Arawn-Davies/makar/issues/121)              |
|    16 | execve + wait4 + `TASK_ZOMBIE` (16a-b)                 | ✅ shipped (#166)           | rolled into [#121](https://github.com/Arawn-Davies/makar/issues/121)              |
|    17 | UTF-8 terminal                                         | ⏭ deferred                  | [#148](https://github.com/Arawn-Davies/makar/issues/148)                          |
|    18 | `ps`-style task listing (covered by `/proc/tasks`)     | ⏭ deferred                  | [#147](https://github.com/Arawn-Davies/makar/issues/147)                          |
|    19 | VGA-text fallback per-TTY                              | ⏭ queued                    | [#146](https://github.com/Arawn-Davies/makar/issues/146)                          |
|    20 | Userland shell (`sh.elf`, multi-PR feature, 20a-f)     | 🟢 20a-c shipped, 20g (POSIX A1-A3 pipes/redir/list-ops) shipped via PR #181 | —                                                                                  |
|    21 | COM2 serial-input mode for ui-test runner              | ⏭ planned                   | —                                                                                  |
|    28 | `SYS_PIPE` + `SYS_DUP2` + `FD_KIND_PIPE`               | ✅ shipped ([PR #181](https://github.com/Arawn-Davies/makar/pull/181)) | refcounted `pipe_ring_t`, 4 KiB ring, blocking via `task_yield`. Single-arg `SYS_DUP` + FILE-kind open_file_t refcount still pending. |

### Bash-flavoured shell scripting

Shipped in #163 (May 2026): variables (`NAME=value`, `$VAR`, `$?`),
`if`/`elif`/`else`/`fi`, `while`, `for ... in`, `[ TEST ]`,
`sh script.sh`, `./script.sh`, makbox dispatch restriction,
`datetime`/`date`/`time` builtins.  Not on the slice queue (was a
single PR that didn't fit the slice cadence); covered in
[scripting](scripting.md).

## Active themes

Higher-level groupings that cover several slices and/or issues.

### Userspace + libc

Long-term goal: a self-hosting userspace.  Get a real libc on Makar,
then port `dash` (or extend the in-kernel shell), then bring up TCC
for in-place compile-run.  Detailed plan in
[Userland libc](userland-libc.md).

**TCC port progress (May 2026, v0.9):** all original phases shipped, and
self-hosting now extends to the kernel itself.  `tcc.elf` cross-builds
against the libc shim and lives at `/apps/tcc.elf` on every ISO; userspace
apps (`hello`, `calc`, `sh`, `makbox`) self-rebuild in-OS; the kernel
rebuilds end-to-end with `./build-kernel-tcc.sh` (host) or
`/apps/rebuild-kernel.sh` (in-OS) — see
[rebuilding the kernel](rebuild-kernel.md).  Earlier groundwork:
Phase 1 brought writable `FD_KIND_FILE`, `O_CREAT`/`O_TRUNC`/`O_APPEND`,
`SYS_STAT/FSTAT`, `SYS_READDIR`, growable buffers (`SYSCALL_FILE_MAX` lifted
to 16 MiB).  Phase 2 brought the userspace libc shim: `malloc`/`free`/
`realloc`/`calloc` over `SYS_BRK`, `<ctype.h>`, `strtol`/`atoi`/`strdup`/
`qsort`/`sscanf`/`getenv`, `setjmp`/`longjmp`, a `FILE*` layer with
`snprintf` family.  Coverage: `filetest.elf` + `alloctest.elf` plus
`test_file_fd` ktest suite.  Phases 3+ shipped TCC itself + the
self-rebuild milestones; v1.0 stays gated on the polish phase (full-green
tests, hardened harness, "10× dev experience" tooling).

Slices 15+16 closed the `fork`/`execve`/`wait4` half of [#121](https://github.com/Arawn-Davies/makar/issues/121);
the remaining libc/musl/dash half is still open under the same issue.
Slice 20 (paused) carries the userland-shell follow-on.

### Networking

NIC driver → lwIP → DHCP/DNS → simple HTTP/Telnet daemons.

Open:
- [#122 — RTL8139 + lwIP + DHCP/DNS](https://github.com/Arawn-Davies/makar/issues/122)
  — full stack epic.
- [#141 — IP/TCP over NE2000 or virtio-net](https://github.com/Arawn-Davies/makar/issues/141)
  — alternative driver target (NE2000/virtio) for the same stack.

### Makar × Medli interop

Sharing data + binaries with the Medli project.

Open:
- [#138 — COM loader (flat DOS-style binaries)](https://github.com/Arawn-Davies/makar/issues/138)
- [#139 — Serial shell over COM1](https://github.com/Arawn-Davies/makar/issues/139)
- [#140 — MXF executable format (joint binary format)](https://github.com/Arawn-Davies/makar/issues/140)
- [#142 — Login + user accounts (`System\Data\usrinfo.sys`)](https://github.com/Arawn-Davies/makar/issues/142)
- [#143 — INI-style config and data exchange](https://github.com/Arawn-Davies/makar/issues/143)

Background: [Makar × Medli](makar-medli.md).

### Kernel hardening

Things the kernel needs to be properly preemptive + signal-aware.

Shipped (PR #154):
- [#144 — Linux-style signal subsystem](https://github.com/Arawn-Davies/makar/issues/144)
  (slice 8) — per-task handler table, scheduler-driven default-terminate
  delivery, `SYS_KILL(37)` / `SYS_SIGNAL(48)` / `SYS_SIGRETURN(119)`,
  ring-3 trampoline so user-installed handlers actually run, Ctrl+C →
  SIGINT migration (removed `g_sigint` / `keyboard_sigint_consume`),
  `task_t.unkillable` flag protecting idle + shell tasks, `maktop.elf`
  htop-style task viewer with F9 signal picker.
- [#145 — Preemption hardening](https://github.com/Arawn-Davies/makar/issues/145)
  (slice 9) — `in_schedule` re-entrancy guard cleared before
  `task_switch`, `irq_save_disable` / `irq_restore` wrapping the
  critical section, per-task `kticks` accounting in `/proc/tasks`,
  runtime-tunable `g_sched_quantum` (`sched_quantum` shell builtin),
  `test_preempt` busy-loop ktest.

### Filesystem & devices

The on-disk FAT32 layout adopts Medli's canonical structure
(`/Users/`, `/Apps/x86/`, `/System/Data|Logs|Libraries|Modules/`,
`/Temp/` — see [Medli's `Paths.cs`](https://github.com/Arawn-Davies/Medli/blob/main/Medli/Common/Paths.cs)
and [Makar × Medli](makar-medli.md)).  Alongside it, the kernel
exposes Unix-style synthetic mounts so userspace tools have a uniform
handle on drivers and devices.

Open:
- [#151 — Adopt Medli-compatible `/Users/` `/Apps/` `/System/` layout](https://github.com/Arawn-Davies/makar/issues/151)
  (migrate today's flat `/apps/` to Medli's canonical FAT32 structure
  from [`Paths.cs`](https://github.com/Arawn-Davies/Medli/blob/main/Medli/Common/Paths.cs)
  so user data + binaries move between Makar and Medli without
  translation)
- [#152 — Proper HDD install with GRUB2 (Ubuntu/Arch-style)](https://github.com/Arawn-Davies/makar/issues/152)
  (today's in-kernel `install` is fragile: whole-disk format, copied
  `core.img`, no `grub.cfg` generation, no module verification.  Fix
  it into a real installer — depends on #149/#150/#151)

Already shipped:
- `/proc/` synthetic filesystem with `cpuinfo`/`meminfo`/`tasks`/`uname`/`rtc`
  (#129)
- FAT32 R/W; disk filesystems under `/mnt` (`/mnt/root`, `/mnt/boot`,
  `/mnt/cdrom`); `mount /dev/hdaN /mnt/<name>`; flush + unmount on
  shutdown/reboot
- **#150 — `/dev/` synthetic VFS**: raw block devices `/dev/hda[N]` +
  `/dev/cdrom` ([devfs](kernel/devfs.md)); byte-addressed sector I/O
- **#149 — fdisk**: `fdisk.elf` MBR editor reads/writes the partition table
  through `/dev/hda`

### Display + terminal

Open:
- [#146 — VGA-text fallback per-TTY backing buffers](https://github.com/Arawn-Davies/makar/issues/146)
  (slice 19)
- [#148 — UTF-8 terminal with ASCII fallback](https://github.com/Arawn-Davies/makar/issues/148)
  (slice 17, deferred)

### Shell

Open:
- [#147 — `ps`-style task listing](https://github.com/Arawn-Davies/makar/issues/147)
  (slice 18; today's `cat /proc/tasks` covers this)
- Slice 20 — userland `sh.elf` (multi-PR feature, paused)

(`serial shell over COM1` lives under the Makar×Medli theme as [#139](https://github.com/Arawn-Davies/makar/issues/139).)

## What's already shipped

See [history](history.md) for the changelog.  Highlights:

- Kernel: paging, heap, scheduler (preemptive 100 Hz), per-task page
  directories, ring-3 lifecycle proven by ktest.
- Storage: FAT32 R/W, ISO 9660, IDE PIO, VFS with auto-mount, synthetic
  `/proc`.
- Userspace: ELF loader, ring-3 `iret`, full POSIX **fork + execve +
  wait4** (slices 15-16 with copy-on-write page-table clone), syscall
  surface (Linux i386 subset + Makar extensions 200–218), `makbox`
  busybox-style multicall, apps (`calc`, `vix`, `fdisk`, `basic`,
  `kbtester`, `forktest`, `execvetest`, …).
- Display: VESA framebuffer (720p default), VGA 80×50 fallback, per-TTY
  backing grids, **makmux** status bar, four independent TTYs with
  Alt+F1–F4.
- Tooling: single-entrypoint `run.sh`, ccache toolchain image,
  build-once fan-out CI (4 parallel jobs including a UI-test job).

## Tracking conventions

- **Slices** are kernel/userspace work units numbered in the table
  above.  Each slice should land as one focused PR.
- **Issues** use `feat(<scope>): <topic>` titles for consistency with
  commit messages.  Scopes seen so far: `kernel`, `shell`, `display`,
  `networking`, `makbox`.
- **PRs** reference the issue they close in the body
  (`Closes #N` or `Fixes #N`).  History lives in `docs/history.md`,
  not in PR descriptions — once the PR is merged, the changelog entry
  is the canonical record.
