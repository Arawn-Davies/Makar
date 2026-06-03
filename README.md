# Makar

[![Build & Test](https://github.com/Arawn-Davies/makar/actions/workflows/build-test.yml/badge.svg)](https://github.com/Arawn-Davies/makar/actions/workflows/build-test.yml)
[![Release](https://github.com/Arawn-Davies/makar/actions/workflows/release.yml/badge.svg)](https://github.com/Arawn-Davies/makar/actions/workflows/release.yml)

Makar is a bare-metal i686 hobby OS written in C and AT&T assembly, booted by
GRUB Multiboot 2, built around a Linux i386-shaped userspace ABI, and tested in
QEMU. It is the C/GCC sibling of
[Medli](https://github.com/Arawn-Davies/Medli): the projects share OS concepts,
filesystem vocabulary, and long-term compatibility goals while using different
implementation stacks.

This README is an entry point, not the manual. Current implementation detail
lives in [`docs/`](docs/) and in the source.

Current kernel version: `0.9.5`
([`src/kernel/include/kernel/version.h`](src/kernel/include/kernel/version.h)).

## What It Currently Is

Makar boots into a graphical VESA environment with virtual terminals, ring-3
ELF userspace, a userspace shell, a small hosted libc, an in-OS TinyCC, FAT32
and ext2 storage, copy-on-write `fork`, `execve`, `wait4`, pipes, signals,
anonymous `mmap`, i386 TLS, and x87/SSE task-state handling.

For the authoritative current-state summary, read
[`docs/index.md`](docs/index.md).

## Quick Start

```sh
./run.sh iso build
./run.sh iso boot
./run.sh ktest
./run.sh kbtest
./run.sh kbtest gui
./run.sh gui all-tests
```

`./run.sh` with no arguments prints the supported command surface. The build
uses the host toolchain when available and otherwise wraps the project Docker
image.

Detailed build and test behavior:

- [`docs/building.md`](docs/building.md)
- [`docs/testing.md`](docs/testing.md)
- [`docs/wsl2.md`](docs/wsl2.md)

## Where To Read Next

| Topic | Document |
|---|---|
| Current architecture overview | [`docs/index.md`](docs/index.md) |
| Build/run/test commands | [`docs/building.md`](docs/building.md) |
| Test matrix and expected markers | [`docs/testing.md`](docs/testing.md) |
| POSIX compatibility status | [`docs/posix.md`](docs/posix.md) |
| Syscall ABI and syscall table | [`docs/syscalls.md`](docs/syscalls.md) |
| Userspace libc and static-musl bring-up | [`docs/userland-libc.md`](docs/userland-libc.md) |
| In-OS TinyCC status | [`docs/tcc.md`](docs/tcc.md) |
| Kernel internals | [`docs/internals.md`](docs/internals.md) |
| Per-subsystem reference | [`docs/kernel/`](docs/kernel/) |
| Roadmap | [`docs/roadmap.md`](docs/roadmap.md) |
| Makar and Medli | [`docs/makar-medli.md`](docs/makar-medli.md) |
| Acknowledgements and licensing notes | [`LICENSES/THANKS.md`](LICENSES/THANKS.md) |

## Repository Map

```text
src/kernel/      kernel, architecture code, drivers, filesystems, scheduler
src/libc/        freestanding kernel-side libc fragment
src/userspace/   ring-3 apps, scripts, syscall wrappers, hosted libc shim
tests/           GDB boot-test helpers
docs/            GitHub Pages documentation
LICENSES/        third-party notes, font origins, acknowledgements
toolchain/       static-musl cross-toolchain experiments
vendor/tinycc/   vendored TinyCC source
```

## Development Notes

- CI is intentionally path-filtered: Build & Test runs only for `src/**` or
  `run.sh` changes.
- In-guest tests assert on serial markers. The old host-driven keyboard/HMP
  harness is gone; use the script drivers or `kbtest`.
- Commit one discrete work item at a time.
- Do not add AI-attribution footers to commits.

## Licence

Original Makar source is MIT licensed; see [`LICENSE`](LICENSE). Third-party
and provenance notes are in [`LICENSES/`](LICENSES/), especially
[`LICENSES/THANKS.md`](LICENSES/THANKS.md).
