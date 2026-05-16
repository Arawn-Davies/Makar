---
title: Makar × Medli
nav_order: 5
---

# Makar × Medli — Co-operation roadmap

Makar and [Medli](https://github.com/Arawn-Davies/Medli) are two
independent implementations of the same operating-system concept,
developed in parallel by the same author. They are **siblings**, not
layers: Makar is not a bootloader or HAL for Medli; it is a ground-up
re-implementation of the same OS idea in a lower-level language, sharing
UX conventions, command vocabulary, and (eventually) binary formats.

|  | Makar | Medli |
|---|---|---|
| **Language** | C (i686-elf-gcc, `-std=gnu11`). No C++ anywhere — no `extern "C"`, no managed runtime. | C# / X#, .NET via Cosmos / IL2CPU |
| **Runtime** | Bare metal — manual PMM + paging + heap | Cosmos managed kernel + GC |
| **Target** | i686, ELF32, multiboot 2 (GRUB) | x86, Cosmos PE |
| **Repo** | [Arawn-Davies/Makar](https://github.com/Arawn-Davies/Makar) | [Arawn-Davies/Medli](https://github.com/Arawn-Davies/Medli) |
| **Path separator** | `/` (Unix) | `\` (DOS-style, `Paths.Separator = @"\"`) |
| **Current version** | 0.5.0 | (see Medli repo) |

The path-separator divergence is **intentional** — Makar follows
Linux/Unix conventions because that is what the C tooling, FAT32 LFN
support, and POSIX-shaped syscalls expect. A shared filesystem layout
is still possible (see "Filesystem layout" below); paths are translated
at the shell layer, not stored differently on disk.

---

## Shared identity

Both OSes present the same conceptual environment to a user at a
terminal or serial console:

- A **command shell** with a consistent command vocabulary (`ls`, `cd`,
  `cat`, `mkdir`, `mount`, `ps`-equivalent, etc.).
- A **daemon/service model** (Medli has `Daemon` objects; Makar has
  preemptive kernel tasks today, will gain a daemon abstraction once a
  service-registration API lands).
- A **vi-style editor** (see VIX history below).
- A common **filesystem layout** target.
- A **user account system** (Medli only today; Makar deferred until
  signals + per-task FD table land).

---

## VIX history (was VICS)

Medli's text editor was named **VICS** — a contraction of "vi C-Sharp".
When the editor was ported to Makar it kept that name despite being
written in straight C, which made the acronym a lie. In May 2026 the
Makar port was renamed **VIX** to remove the language-specific
reference and leave the name free for a future round-trip back to
Medli without either project's acronym becoming stale.

| Property | VICS (Medli, original) | VIX (Makar, current) | VICS (Medli, future) |
|---|---|---|---|
| Language | C# | C | C# (will be rebased on VIX semantics) |
| Editor model | Single-file modal vi | Single-file modal vi | Same |
| Status row | Bottom of pane | Bottom of pane | Same |
| Line numbers | No | Yes (4-digit gutter, `~` past EOF) | Planned |
| Word wrap | No | Yes (visual, in-pane) | Planned |
| Cursor | Static block | Flashing block (`vesa_tty_set_caret_style(2)`) | Planned |
| Source | Medli/Editors/Vics.cs | `src/userspace/vix.c`, `src/kernel/arch/i386/proc/vix.c` | — |

When Medli's VICS is rewritten against VIX's behaviour the editor
becomes identical on both OSes; the rename is the visible part of that
joint specification.

---

## Filesystem layout target

Medli defines the canonical directory structure; Makar adopts it modulo
the path separator. The on-disk FAT32 layout is identical — the shell
just presents `/` to the user instead of `\`.

```
/                     (volume root,    0:\ on Medli)
├── Users/
│   └── Guest/
├── root/
├── Apps/
│   ├── Temp/
│   └── x86/          ← native Makar ELF binaries live here
├── System/
│   ├── Data/
│   ├── Logs/
│   ├── Libraries/
│   └── Modules/
└── Temp/
```

Makar currently uses a flatter `apps/` directory at FAT32 root (see
`isodir/apps/` and `/hd/apps/` PATH lookup); the layout above is the
target once the userspace catalogue grows past a handful of ELFs.

---

## Binary compatibility goals

The aim is for a userspace program to run on either OS without
recompilation where technically feasible.

### Native Makar binaries (`/Apps/x86/`)
Makar produces ELF32 statically-linked binaries. The kernel's
`elf_exec` (`src/kernel/arch/i386/proc/elf.c`) maps and runs them. A
Medli ELF-compat shim is plausible via `Medli.Plugs` or a native-exec
daemon.

### Medli Executable Format (MEF)
Medli supports two formats today:
- **COM** — flat binary, fixed-address, DOS-compatible.
- **MEF** — native managed format, Cosmos-runtime-dependent.

COM is the lowest common denominator: a Makar COM loader could run
simple Medli COM programs (and vice versa). MEF stays Medli-only.

### Proposed shared format: MXF
A lightweight ELF-inspired header with a common magic, a single text
section of native i686 code, and (optionally) relocation tables. Both
kernels would parse the header; each would execute the payload in its
own sandboxing model.

---

## Makar kernel milestones

Tracking is in `CLAUDE.md` under "Slice queue". Mapped to Medli
co-operation:

### Shipped (relevant to Medli interop)

- [x] **PS/2 keyboard** — IRQ 1, full set-1 + `e0` decoder, layered
      scancode→keycode→sentinel→router pipeline, per-task SPSC rings
      (PR #124).
- [x] **Shell** — interactive REPL on each of 4 TTYs, inline editing,
      history, tab completion across all mounted filesystems, Ctrl+C
      sigint.
- [x] **ATA/IDE PIO** — 28-bit LBA, 4-drive scan, ATAPI eject.
- [x] **MBR + GPT** — read and write; `lspart`, `mkpart mbr`,
      `mkpart gpt`.
- [x] **FAT32 R/W** — mount, ls, cat, cd, mkdir, mkfs, file
      delete/rename, directory delete (PR #120).
- [x] **ISO 9660** — read-only auto-mount at `/cdrom` from ATAPI.
- [x] **Process model** — preemptive round-robin scheduler at 100 Hz
      with `SCHED_QUANTUM = 4` ticks (40 ms slice), per-task page
      directory, dead-task PD reaper (PRs #123, #128).
- [x] **ELF loader** — loads and executes ring-3 static binaries with
      argc/argv.
- [x] **VIX text editor** — ported from Medli VICS; renamed during the
      port (see "VIX history" above).
- [x] **Per-TTY screen buffers** — `vt_buf_t` backing grid per TTY, FB
      paint only when focused, repaint deferred out of IRQ context.
      Status bar at bottom row (this PR series).
- [x] **Synthetic `/proc`** — `/proc/{cpuinfo,meminfo,tasks,uname}` as
      read-only files generated on demand.
- [x] **Serial console** — Linux-style: `console=ttyS0` cmdline opts
      into TTY-mirroring; default keeps COM1 quiet (dmesg-only).

### In flight

- [ ] **Per-task FD table** — replace global keyboard owner +
      placeholder `fd_table` with a real per-task array (Slice 14;
      see CLAUDE.md). Prerequisite for pipe(2), dup(2), and any libc port.
- [ ] **Linux-style signals** — `task->sig_pending` exists; need
      sigaction table, `kill()` syscall, default disposition (Slice 8).
- [ ] **VFS `task->cwd` authoritative** — drop the global `s_cwd` in
      `vfs.c` (Slice 15).

### Medium-term

- [ ] **COM loader** — for cross-OS COM binaries.
- [ ] **MXF executable format** — joint specification with Medli.
- [ ] **fork() / posix_spawn** — COW page-table clone (Slice 12).
- [ ] **User accounts** — `root`, `guest` stored in
      `/System/Data/usrinfo.sys`, format compatible with Medli's
      `usrinfo.sys`.
- [ ] **VGA-fallback per-TTY buffers** — route `tty.c` writes through
      `vt_buf` so VGA text mode gets the same per-TTY isolation as
      VESA (Slice 16).

### Long-term

- [ ] **Network stack** — RTL8139 → lwIP → DHCP/DNS → minimal HTTP
      client. Enables the same daemon-over-TCP model as Medli.
- [ ] **Inter-OS file exchange** — agreed-upon plain-text / INI config
      and log formats readable by both OSes from a shared FAT32 image.

---

## Shared UX conventions

| Convention | Medli | Makar |
|---|---|---|
| Volume root | `0:\` | `/` |
| Path separator | `\` | `/` |
| `ls` / `dir` listing | ✅ | ✅ (`ls`) |
| `cd` | ✅ | ✅ |
| `cat` | ✅ | ✅ |
| `mkdir` | ✅ | ✅ |
| Editor | VICS (C#) | VIX (C; port + rename) |
| User login prompt | ✅ | ⏭ pending account system |
| Serial console | ✅ | ✅ (linux-style; quiet by default) |
| Daemon/service model | ✅ | ⏭ kernel tasks today, no service API yet |
| Shell prompt `user@host /cwd>` | ✅ | ✅ |
| Welcome banner | ✅ | ✅ |
| Version string | `KernelVersion` constant | `MAKAR_VERSION` macro in `include/kernel/version.h` |

---

## Divergences by design

Some differences are intentional and reflect the different language
choices rather than any disagreement about UX.

| Aspect | Makar | Medli |
|---|---|---|
| Memory management | Manual (PMM + paging + heap) | Cosmos GC |
| Interrupt handling | Bare IDT + ISR stubs | Cosmos abstraction |
| Graphics | VESA linear framebuffer (direct pixel writes) | Cosmos `VGAScreen` / bitmap |
| Build system | Makefile + cross-GCC + Docker | .NET SDK + IL2CPU |
| Debug tooling | GDB over QEMU stub | Cosmos debugger / serial |
| Testing | ktest in-kernel + GDB checkpoint suite + headless UI test via QEMU HMP sendkey | (Medli equivalent TBD) |
| Path separator | `/` | `\` |

These divergences do not affect user-visible behaviour. They are
implementation details hidden beneath the shared shell, filesystem, and
(eventually) executable-format layer.

---

## How to contribute

1. **Agree on file formats early.** Config files, log formats, and
   executable headers should be designed jointly so neither OS needs to
   change its format later.
2. **Test on QEMU.** Both OSes run under `qemu-system-i386`; use `-hda`
   with a shared FAT32 image to test interop.
3. **Keep the shell vocabulary in sync.** Add new commands to both
   implementations, or document which commands are OS-specific. Makar's
   command list is in `SURVEY.md`.
4. **Use serial as the integration bus.** Both OSes write to COM1; a
   simple test harness can grep both serial logs for the same expected
   output. Makar's UI-test framework (`tests/ui_test.sh`) demonstrates
   the pattern.
