# Building & Running Makar

The entire build and test toolchain runs inside Docker - no cross-compiler or
native GDB is required on the host.  For Windows-specific setup see
[WSL2 guide](wsl2.md).

---

## Prerequisites

| Tool | Purpose |
|---|---|
| `docker` | Runs the CI image that contains the full cross-toolchain |
| `qemu-system-i386` (host, optional) | Used by `run.sh` in preference to Docker QEMU for interactive and test modes; falls back to container QEMU if absent |

---

## Quick start

```sh
# Interactive kernel shell (builds in Docker, QEMU runs on host or in Docker):
./run.sh iso-boot

# Full CI test suite (ktest + GDB boot tests - works with or without host QEMU):
./run.sh iso-test

# Interactive HDD boot:
./run.sh hdd-boot
```

---

## run.sh reference

All build, test, and boot operations go through a single script:

```
./run.sh <mode>
```

### Modes

| Mode | Description | Host requirements |
|---|---|---|
| `iso-boot` | Clean → debug ISO → interactive QEMU | Docker; host QEMU preferred (Docker fallback) |
| `iso-test` | Full CI suite: ktest + GDB boot tests | Docker |
| `iso-ktest-gui` | TEST_MODE ISO → ktest with display window | Docker, host QEMU + display server |
| `iso-release` | Optimised release ISO | Docker |
| `hdd-boot` | Clean → build kernel → HDD image → interactive QEMU | Docker; host QEMU preferred (Docker fallback) |
| `hdd-test` | Clean → build kernel → HDD image → GDB boot test | Docker |
| `hdd-release` | HDD image only | Docker |
| `clean` | Remove all build artefacts | Docker |

### Execution context

`run.sh` picks an execution context automatically, checked in this order:

| Context | Condition | Effect |
|---|---|---|
| **container** | `/.dockerenv` present (GitHub Actions `container:` job, or manual `docker run`) | Steps run directly - no inner `docker run` |
| **docker** | Docker CLI available on the host | Steps are wrapped in `docker run` |
| **native** | `i686-elf-gcc` on the host PATH | Steps run directly - no Docker |
| **none** | None of the above | Error with install hints |

### QEMU strategy

| Step type | Strategy |
|---|---|
| Headless test (ktest, GDB) | Host `qemu-system-i386` preferred; falls back to container QEMU |
| GDB test (needs `gdb-multiarch` too) | Host qemu + gdb-multiarch if both present; otherwise fully in container |
| Interactive boot (`iso-boot`, `hdd-boot`) | Host QEMU preferred; Docker `-it` fallback |
| `iso-ktest-gui` | Host QEMU + display required; errors if absent |

### Environment variables

| Variable | Default | Effect |
|---|---|---|
| `CFLAGS` | *(Makefile default)* | Compiler flags forwarded to the build step |
| `CPPFLAGS` | *(empty)* | Preprocessor flags forwarded to the build step |
| `DOCKER_BIN` | `docker` | Docker CLI binary |
| `DOCKER_IMAGE` | `arawn780/gcc-cross-i686-elf:fast` | Build container image |
| `DOCKER_PLATFORM` | `linux/amd64` | `--platform` flag passed to `docker run` |
| `HDD_IMG` | `makar-hdd.img` | Output filename for interactive HDD builds |
| `HDD_TEST_IMG` | `makar-hdd-test.img` | Output filename for CI HDD test builds |
| `QEMU_DISPLAY` | *(empty)* | Passed to `-display` for `iso-ktest-gui` |

---

## Internal build scripts

These run inside the container and are called by `run.sh` - do not invoke directly.

| Script | What it does |
|---|---|
| `build.sh` | Compiles the kernel and libc into `sysroot/` (parallel via `-j$(nproc)`) |
| `iso.sh` | Calls `build.sh`, then packages into `makar.iso` via `grub-mkrescue` |
| `clean.sh` | Removes `sysroot/`, `isodir/`, and all build artefacts |
| `generate-hdd.sh` | Creates a raw MBR + FAT32 + GRUB 2 HDD image; called by `hdd-boot` / `hdd-test` / `hdd-release` |

Old per-task Docker wrappers are preserved in `scripts/` for reference.

---

## Docker Compose

`docker-compose.yml` is available for workflows that prefer Compose.
Prefer `run.sh` for day-to-day use.

| Service | Command |
|---|---|
| `build` | `bash iso.sh` (release ISO) |
| `build-debug` | `bash iso.sh` with `CFLAGS=-O0 -g3` |
| `test` | `bash run.sh iso-test` (full suite) |

```sh
docker compose run --rm build          # release ISO
docker compose run --rm build-debug    # debug ISO
docker compose run --rm test           # full iso-test suite
```

---

## What the Docker image provides

The `arawn780/gcc-cross-i686-elf:fast` image ships everything needed for building, testing, and HDD image creation:

| Tool / package | Purpose |
|---|---|
| `i686-elf-gcc` / `i686-elf-binutils` | Bare-metal cross-compiler (GCC 13.2, Binutils 2.41) |
| `grub-mkimage`, `grub-file`, `grub-pc-bin` | ISO and HDD GRUB image creation |
| `xorriso`, `mtools` | `grub-mkrescue` ISO packaging |
| `dosfstools` | `mkfs.fat` for FAT32 partition creation |
| `fdisk` / `sfdisk` | MBR partition table writing |
| `qemu-system-i386` | Headless boot testing inside Docker |
| `gdb-multiarch` | GDB with i386 target for boot-checkpoint tests |
| `make`, `build-essential`, `nasm` | Host build tools |

To rebuild and push after adding packages:
```sh
docker buildx build --platform linux/amd64 \
    -t arawn780/gcc-cross-i686-elf:fast \
    -f Dockerfile.compiler --push .
```

---

## QEMU drive layout

`run.sh iso-boot` launches QEMU with two IDE drives:

| IDE slot | Purpose |
|---|---|
| index 0 | Hard disk - 512 MiB raw image (`hdd.img`, auto-created blank) |
| index 2 | Live CD - `makar.iso`, GRUB boots from here (`-boot order=d`) |

`run.sh hdd-boot` attaches only the HDD (`-boot c`, no CD-ROM).

The ISO GDB test (`iso-test` phase 2) adds a 32 MiB FAT32 test disk on index 0 alongside the CD-ROM so the kernel can mount `/hd` and the `hdd_mount` GDB group can be verified on the ISO boot path.

---

## Testing & debugging

See [Testing](testing.md) for the ktest suite, GDB boot-checkpoint tests,
and interactive GDB debugging.
