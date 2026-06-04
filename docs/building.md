---
title: Building & running
parent: Getting started
nav_order: 1
---

# Building and Running Makar

`run.sh` is the single supported entry point for building, booting, and
testing. It chooses Docker, native tools, or the current container depending on
what is available.

Run it with no arguments to print usage:

```sh
./run.sh
```

## Prerequisites

| Tool | Required? | Purpose |
|---|---:|---|
| Docker | yes for the default path | build container with i686 cross-toolchain, GRUB, xorriso, QEMU, GDB |
| `qemu-system-i386` | optional | preferred for interactive and visible runs |
| `gdb-multiarch` or `gdb` | optional | host-side GDB checkpoint runs; Docker fallback exists |
| display server | optional | needed only for visible `gui` / `kbtest gui` modes |
| host network access | optional | for guest networking (`wget`, DNS, ping) reaching a LAN/internet host |

The normal path does not require installing `i686-elf-gcc` on the host.

### Networking requirements

Guest networking uses QEMU **user-mode (slirp)** — no bridge, TAP, or elevated
privileges required, so `iso boot [nic]` / `nettest` work out of the box. The
guest reaches the host at the slirp gateway `10.0.2.2` and any host/LAN/internet
endpoint via NAT; DNS is provided by slirp at `10.0.2.3`.

Two optional extras:

- **External ICMP** (e.g. the guest pinging `1.1.1.1`) needs the host to permit
  unprivileged ICMP sockets. On Linux/WSL2:
  `sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"`. TCP/UDP (so `wget`
  and DNS) work without it. WSL2 does not persist this across `wsl --shutdown`.
- **TLS / HTTPS** is **not** supported in-guest. To fetch from an `https://`
  origin, terminate TLS on the host and serve the file over plain HTTP.

## Quick Start

Build and boot an ISO:

```sh
./run.sh iso boot
```

Build artifacts without booting:

```sh
./run.sh iso build
```

Run the main headless tests:

```sh
./run.sh ktest
./run.sh kbtest
./run.sh iso test
```

Watch all in-guest suites in a visible QEMU window:

```sh
./run.sh gui all-tests
```

## Command Grammar

Current command forms:

```text
./run.sh iso   build | boot | test | release
./run.sh hdd   build | boot | test | release
./run.sh gdb   iso | hdd
./run.sh ktest [graphical]
./run.sh kbtest [gui]
./run.sh gui   libc | incore | ktest | smoke | all-tests
./run.sh clean
```

There are no `ui` or `all` modes anymore. The host-driven UI harness was
removed in favor of in-guest tests.

## ISO Targets

| Command | What it does |
|---|---|
| `./run.sh iso build` | incremental debug build; emits `makar.iso` and `makar-test.iso` |
| `./run.sh iso boot` | builds debug ISO and boots interactively |
| `./run.sh iso test` | builds test ISO, runs ktest, then GDB ISO checkpoints |
| `./run.sh iso release` | optimized release ISO |

`iso boot` is the normal interactive development boot. `iso test` is the
CI-style gate. Keyboard/makmux behavior is covered separately by `kbtest`.

## HDD Targets

| Command | What it does |
|---|---|
| `./run.sh hdd build` | build kernel and HDD test image artifacts |
| `./run.sh hdd boot` | boot from an HDD image |
| `./run.sh hdd test` | run HDD GDB checkpoint test |
| `./run.sh hdd release` | produce release HDD image |

The HDD paths are useful for installer, writable-root, GRUB, FAT32, and ext2
work. ISO paths are faster for day-to-day kernel/userspace testing.

## GDB Targets

| Command | What it does |
|---|---|
| `./run.sh gdb iso` | build and run the ISO GDB checkpoint suite |
| `./run.sh gdb hdd` | build and run the HDD GDB checkpoint suite |

The GDB suites validate early boot, Multiboot state, kernel checkpoints, and
mount/boot conditions that are easier to inspect from the host debugger than
from inside the guest.

## ktest

Headless:

```sh
./run.sh ktest
```

Visible:

```sh
./run.sh ktest graphical
```

This boots `test_mode test=ktest` and checks the serial marker:

```text
KTEST_RESULT: PASS
```

## kbtest

Headless:

```sh
./run.sh kbtest
```

Visible:

```sh
./run.sh kbtest gui
```

This boots the normal shell with `kbtest` on the kernel command line. The
kernel injects keys inside the guest and checks serial `KBTEST:` markers. It
is the dedicated coverage for keyboard routing, Ctrl-C, makmux, VT switching,
and app tabs.

## Visible In-Guest Suites

```sh
./run.sh gui ktest
./run.sh gui incore
./run.sh gui libc
./run.sh gui smoke
./run.sh gui all-tests
```

These use test-mode boot arguments and a visible QEMU display. They do not
type through the host. The guest runs scripts and emits serial markers.

Suites:

| Suite | Guest test |
|---|---|
| `ktest` | kernel suite |
| `incore` | userspace binary exit-status checks |
| `libc` | in-OS TCC/libc compile matrix (`libc-tcc`) |
| `smoke` | shell/VFS/app smoke tests |
| `all-tests` | all of the above |

## Execution Context Selection

For build steps, `run.sh` checks:

1. running inside a container with `i686-elf-gcc`
2. Docker CLI available
3. native `i686-elf-gcc` available
4. otherwise error with install hints

For QEMU/GDB steps, host tools are preferred where they make sense. Docker is
used as a fallback for headless paths. Visible GUI paths require host QEMU with
a display backend.

## Important Environment Variables

| Variable | Default | Effect |
|---|---|---|
| `DOCKER_IMAGE` | `makar-build:local` | local build image layered with ccache |
| `DOCKER_UPSTREAM_IMAGE` | `arawn780/gcc-cross-i686-elf:fast` | upstream toolchain base |
| `DOCKER_BIN` | `docker` | Docker CLI |
| `DOCKER_PLATFORM` | `linux/amd64` | platform passed to Docker |
| `HDD_IMG` | `makar-hdd.img` | interactive HDD image path |
| `HDD_TEST_IMG` | `makar-hdd-test.img` | CI/test HDD image path |
| `MAKAR_HDD_SIZE_MB` | `96` | interactive HDD image size |
| `MAKAR_HDD_TEST_SIZE_MB` | `96` | test HDD image size |
| `MAKAR_USE_KVM` | `0` | opt into KVM if `/dev/kvm` is usable |
| `QEMU_DISPLAY` | unset | display backend for visible runs |
| `KERNEL_ARGS` | unset | extra kernel command-line args for supported paths |

KVM is disabled by default because deterministic TCG behavior is more useful
for CI and GDB checkpoint tests.

## Build Products

| File/directory | Purpose |
|---|---|
| `makar.iso` | interactive ISO |
| `makar-test.iso` | test-mode ISO |
| `src/kernel/makar.kernel` | kernel ELF |
| `sysroot/` | staged kernel/sysroot artifacts |
| `isodir/` | staged ISO tree |
| `src/userspace/*.elf` | userspace app binaries |
| `src/userspace/*.d` | generated dependency files, ignored |

Use:

```sh
./run.sh clean
```

to remove build artifacts.

## Toolchain Images

The default Docker path uses:

- `arawn780/gcc-cross-i686-elf:fast` as the upstream toolchain image
- `makar-build:local` as a local ccache-enabled layer

The image provides:

- `i686-elf-gcc`
- binutils
- GRUB tools
- xorriso/mtools
- QEMU i386
- GDB multiarch
- filesystem tools for FAT32/ext2 image work

The separate `toolchain/` directory is for static i386 musl experiments, not
for building the kernel itself.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Docker permission denied | user cannot access Docker socket | start Docker or add user to Docker group |
| visible GUI fails | host QEMU/display missing | use headless command or install display-capable QEMU |
| stale userspace behavior after header edits | old object did not rebuild | dependency tracking should now handle this; try `./run.sh clean` if in doubt |
| GDB checkpoint flaky under KVM | KVM timing/debug behavior | leave `MAKAR_USE_KVM=0` |
| writes fail on live ISO paths | ISO9660 is read-only | write to `/tmp` or an installed writable root |
