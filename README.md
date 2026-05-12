# Makar

[![CI](https://github.com/Arawn-Davies/Makar/actions/workflows/ci.yml/badge.svg)](https://github.com/Arawn-Davies/Makar/actions/workflows/ci.yml)
[![Release](https://github.com/Arawn-Davies/Makar/actions/workflows/release.yml/badge.svg)](https://github.com/Arawn-Davies/Makar/actions/workflows/release.yml)

> *We're standing on the shoulders of giants, and none of this would be
> possible without the hard work and contributions of the thousands of
> developers throughout time.*
>
> *Homage to the Kernel. Homage to the Contributors. Homage to the Source
> Control.*
>
> See [`LICENSES/THANKS.md`](LICENSES/THANKS.md) for the full
> acknowledgements.

A bare-metal **i686 hobby OS** written in C, booted via GRUB Multiboot 2.
Makar is the **C / GCC sibling** of
[Medli](https://github.com/Arawn-Davies/Medli) - two independent
implementations of the same OS concept, sharing a command vocabulary,
filesystem layout, and long-term binary format goals.

Self-contained: kernel, libc fragment, ring-3 userspace, ELF loader, **four
independent TTYs (Alt+F1–F4 to switch)**, and an in-kernel `vi`-style
editor - all under one repo.

Built with the [`i686-elf-gcc`](https://github.com/Arawn-Davies/quick-i686)
cross-compiler. Designed and tested in QEMU; runs on real hardware once
installed to disk.

## Sibling project - Medli

[Medli](https://github.com/Arawn-Davies/Medli) is the C# / Cosmos
counterpart of Makar. The shared lineage matters: both projects evolve in
lockstep at the design level (UX, on-disk formats, service contracts)
while exploring how each language and runtime shapes the implementation.
See the [Makar × Medli roadmap](docs/makar-medli.md) for the full
co-operation plan.

## Current state

Boots to an interactive 720p VESA shell with **4 independent TTYs**
(Alt+F1–F4 to switch). Each TTY is its own preemptive kernel task with a
private kernel stack and (for ring-3 programs) its own page directory.

| Subsystem | Notes |
|---|---|
| **Boot** | GRUB Multiboot 2 + 5-second menu (Makar OS / chainload next device). Multiboot 2 cmdline parsed for runtime flags. |
| **Display** | VESA framebuffer (Bochs VBE, 720p default), VGA 80×50 fallback. Pane API (`vesa_pane_t`) for split-screen. |
| **Multi-TTY** | 4 independent shell tasks (`shell0`–`shell3`), **Alt+F1–F4** to switch focus, per-pane redraws on `KEY_FOCUS_GAIN`. |
| **VICS editor** | Pane-aware vi-style editor (FUZIX/ELKS-inspired). Resolution-agnostic. |
| **Storage** | FAT32 (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. Auto-mount at `/hd` and `/cdrom`. Read+write+delete+rename on FAT32. |
| **Memory** | PMM bitmap allocator, paging (256 MiB identity + per-task 4 KiB user pages), kernel heap (`kmalloc`/`kfree`/`krealloc`). |
| **Tasking** | **Preemptive** round-robin scheduler. PIT at **100 Hz**, `SCHED_QUANTUM = 4` ticks → 40 ms time slice. Per-task `pid`, `cwd`, `tty`, fd-table placeholder, signal bitmasks. |
| **Userspace** | Ring-3 via `iret`. ELF loader with argc/argv. Apps: `hello`, `echo`, `calc`, `ls`, `vics`, `diskinfo`, `rm`, `mv`, `cp`. |
| **Syscalls** | Linux i386 ABI subset over `int 0x80` - `SYS_EXIT`, `SYS_READ`, `SYS_WRITE` (fd 1 = VGA, fd 2 = VGA + COM1 serial), `SYS_OPEN`, `SYS_CLOSE`, `SYS_LSEEK`, `SYS_BRK`, `SYS_DEBUG`, `SYS_YIELD`, plus Makar extensions for terminal/file ops and `SYS_WRITE_SERIAL` (211). |
| **Shell** | Inline editing, history (16 entries), tab completion, Ctrl+C. Built-ins: `ls`, `cd`, `cat`, `cp`, `mv`, `mkdir`, `rm`, `rmdir`, `mount`, `meminfo`, `uptime` (humanised h/m/s), `lsdisks`, `lspart`, `mkpart`, `readsector`, `exec`, `ktest`, `ring3test`, `vicstest`. `lsman` / `man <cmd>` for help. |
| **Drivers** | Serial (16550 UART, 38400 baud), PIT, PS/2 keyboard (set 1 + e0 extended), ATA/IDE PIO (28-bit LBA, 4 drives), MBR + GPT partition tables. |
| **Debug** | INT 1 / INT 3 GDB-friendly handlers, kernel panic screen, ktest harness with VESA + serial output. |

## Quick start

```sh
./run.sh iso-boot       # build & run interactively in QEMU (host or Docker)
./run.sh iso-test       # full CI suite: ktest + GDB boot-checkpoint tests
./run.sh hdd-boot       # build & run from a 512 MiB FAT32 HDD image
./run.sh clean
```

The build is wrapped in Docker (`arawn780/gcc-cross-i686-elf:fast`) - no
host cross-compiler required. If `i686-elf-gcc` is on your PATH it'll be
used directly; otherwise Docker takes over transparently.

## Documentation

| Guide | |
|---|---|
| [Building](docs/building.md) | Prerequisites, build scripts, Docker, Compose |
| [Testing](docs/testing.md) | ktest harness, GDB boot-test groups |
| [WSL2](docs/wsl2.md) | Windows development via WSL2 + Docker Desktop |
| [Userland libc](docs/userland-libc.md) | Roadmap to musl/uClibc-ng/dash |
| [Makar × Medli](docs/makar-medli.md) | Sibling-project roadmap |
| [Kernel subsystems](docs/index.md) | Per-driver / per-module reference |

## Roadmap (near-term)

Tracked in [`CLAUDE.md`](CLAUDE.md). Active branch:
[`feat/tty-multitasking`](https://github.com/Arawn-Davies/Makar/tree/feat/tty-multitasking)
- preemptive scheduler hardening, per-task refactor (CWD/TTY/FD migration
from globals), Linux-style signals, fork() readiness, full PS/2 keyboard
rewrite.

<!-- ci-trigger-test: this comment is intentionally docs-only to verify the workflow path filter skips this commit. -->
