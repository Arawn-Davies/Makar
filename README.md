# Makar

[![Build & Test](https://github.com/Arawn-Davies/makar/actions/workflows/build-test.yml/badge.svg)](https://github.com/Arawn-Davies/makar/actions/workflows/build-test.yml)
[![Release](https://github.com/Arawn-Davies/makar/actions/workflows/release.yml/badge.svg)](https://github.com/Arawn-Davies/makar/actions/workflows/release.yml)

> *We're standing on the shoulders of giants, and none of this would be
> possible without the hard work and contributions of the thousands of
> developers throughout time.*
>
> *Homage to the Kernel. Homage to the Contributors. Homage to the Source
> Control.*
>
> See [`LICENSES/THANKS.md`](LICENSES/THANKS.md) for the full
> acknowledgements.

A bare-metal **i686 hobby OS** written in C (strictly C — no C++, no
managed runtime), booted via GRUB Multiboot 2. Makar is the
**C / GCC sibling** of [Medli](https://github.com/Arawn-Davies/Medli) -
two independent implementations of the same OS concept, sharing a
command vocabulary, filesystem layout, and long-term binary format
goals. Current version: **0.9.0** (see `include/kernel/version.h`) —
the kernel self-host milestone: the bootable Multiboot 2 kernel ELF is
now built with our shipped TCC against the vendored source tree
(`./build-kernel-tcc.sh` on the host, `/apps/rebuild-kernel.sh` inside
Makar).  Builds on v0.8's in-OS TCC: `tcc.elf` on bare metal, `calc.elf`
and `sh.elf` self-rebuilding from in-tree source, and ring-3 page faults
delivering SIGSEGV instead of panicking the kernel.  See
[`docs/rebuild-kernel.md`](docs/rebuild-kernel.md).

Self-contained: kernel, libc fragment, ring-3 userspace, ELF loader, **four
independent TTYs (Alt+F1–F4 to switch)**, **in-OS TinyCC compiler**
(`tcc.elf`), an experimental **ring-3 shell** (`sh.elf`) that runs
alongside the in-kernel shell, and an in-kernel `vi`-style editor — all
under one repo.

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
| **VIX editor** | Pane-aware vi-style editor (FUZIX/ELKS-inspired). Resolution-agnostic. |
| **Storage** | FAT32 + **ext2** (HDD/USB) + ISO 9660 (CD-ROM) via IDE PIO. **HDD root layout** (v0.8): the volume hosting `/usr/lib/crt0.o` is elevated to `/` so `/usr`, `/etc`, `/home`, `/bin`, `/src` resolve at the Linux paths; a FAT32 mount named "boot" is elevated to `/boot`; `/mnt` shows only manually mounted volumes. `mount /dev/hdaN /mnt/<name>` still works for ad-hoc mounts. `mkfs.fat32` / `mkfs.ext2`. |
| **Memory** | PMM bitmap allocator, paging (256 MiB identity + per-task 4 KiB user pages), **32 MiB kernel heap** (`kmalloc`/`kfree`/`krealloc`). |
| **Tasking** | **Preemptive** round-robin scheduler. PIT at **100 Hz**, `SCHED_QUANTUM = 4` ticks → 40 ms time slice. Per-task `pid`, `cwd`, `tty`, fd-table placeholder, signal bitmasks. **Ring-3 page faults / GPFs now deliver SIGSEGV** and reap the offender via `task_exit` — userspace bugs no longer panic the kernel. |
| **Userspace** | Ring-3 via `iret`. ELF loader with argc/argv. Full POSIX fork + execve + wait4 (COW). Apps: `hello`, `calc`, `vix`, `diskinfo`, `fdisk`, `cfdisk`, `basic`, `clock`, `maktop`, `kbtester`, `makbox` (busybox-style `ls`/`cat`/`cp`/`mv`/`rm`/`rmdir`/`echo`/`pwd`), **`sh.elf`** (ring-3 shell MVP — exec it from any kernel shell prompt), **`tcc.elf`** (in-OS TinyCC). |
| **Syscalls** | Linux i386 ABI subset over `int 0x80` — `SYS_EXIT`, `SYS_FORK`, `SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, `SYS_CLOSE`, `SYS_EXECVE`, `SYS_CHDIR` (v0.8), `SYS_LSEEK`, `SYS_BRK`, `SYS_WAIT4`, `SYS_STAT`, `SYS_FSTAT`, `SYS_READDIR`, `SYS_KILL`, `SYS_SIGNAL`, plus Makar extensions for terminal/file ops, `SYS_GETCWD`, `SYS_WRITE_SERIAL`. |
| **Shell** | Inline editing, history (16 entries), **zsh-style tab cycling** (first Tab → longest common prefix; subsequent Tabs cycle matches; any non-Tab key commits), Ctrl+C. Built-ins: `ls`, `cd`, `cat`, `cp`, `mv`, `mkdir`, `rm`, `rmdir`, `mount`, `meminfo`, `uptime`, `lsdisks`, `lspart`, `mkpart`, `readsector`, `exec`, `ktest`, `ring3test`, `vixtest`. `lsman` / `man <cmd>` for help. |
| **Compiler** | **In-OS TinyCC** (`/apps/tcc.elf`, v0.9.27, cross-built and shipped on every ISO). Sysroot at `/usr/{include,lib}` with `crt0.o` + `libc.a`. `tcc hello.c -o hello.elf` compiles to a Makar-loadable ELF. Verified self-host: `tcc /src/userspace/calc.c -o /tmp/calc.elf` and `tcc /src/userspace/sh.c -o /tmp/sh.elf` rebuild + run correctly via `./run.sh iso test`. |
| **Drivers** | Serial (16550 UART, 38400 baud), PIT, PS/2 keyboard (set 1 + e0 extended), ATA/IDE PIO (28-bit LBA, 4 drives), MBR + GPT partition tables. |
| **Debug** | INT 1 / INT 3 GDB-friendly handlers, kernel panic screen, ktest harness with VESA + serial output. |

## Quick start

```sh
./run.sh iso boot       # build & run interactively in QEMU (host or Docker)
./run.sh iso test       # full CI suite: ktest + in-guest script drivers + GDB boot-checkpoint
./run.sh hdd boot       # build & run from a 512 MiB FAT32 HDD image
./run.sh hdd test       # HDD-only GDB boot test (no CD-ROM)
./run.sh kbtest                   # in-guest key-injection tests (headless; KBTEST serial markers)
./run.sh kbtest gui               # same, visible window (watch injection drive the shell)
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

Tracked in the [roadmap](docs/roadmap.md) under "Slice queue". Recent
shipping (May 2026):

- **v0.9 kernel self-host milestone** ✅ — `./build-kernel-tcc.sh` (host)
  builds a valid Multiboot 2 kernel ELF from the vendored source using
  only our shipped TCC; `/apps/rebuild-kernel.sh` runs the same recipe
  inside Makar.  Boot banner reports build origin (`gcc-host` /
  `tcc-host` / `tcc-in-os`).  See `docs/rebuild-kernel.md`.
- **v0.8 in-OS TCC milestone** ✅ — `tcc.elf` ships on every ISO;
  `calc.elf` and `sh.elf` self-rebuild via `./run.sh iso test`.
- **HDD root layout** ✅ — `/usr`, `/etc`, `/home`, `/boot` resolved via
  rootfs/bootfs prefix probes; `/mnt` only shows manual mounts.
- **Ring-3 page faults → SIGSEGV** ✅ — userspace bugs no longer panic the
  kernel; panic screen now names the faulting task/pid/ring when ring-0.
- **Zsh-style tab cycling** ✅ — first Tab extends to longest common prefix;
  subsequent Tabs cycle matches in place.
- **Ring-3 shell MVP** ✅ — `/apps/sh.elf` coexists with the in-kernel shell.

Next on deck:

- **Lift more shell out of the kernel** — pipe(2) / job control inside
  `sh.elf`; PATH lookup; eventually drop the in-kernel shell entirely.
- **x87 FPU init** — unblocks BASIC floats + TCC self-compile of `tcc.c`.
- **Streaming file I/O** (`open_file_t` refactor) — replace eager kmalloc
  load so files >16 MiB stream through reads/writes.

<!-- ci-trigger-test: this comment is intentionally docs-only to verify the workflow path filter skips this commit. -->
