---
title: TCC in-OS — feasibility spike (shipped)
parent: Development
nav_order: 2
---

# TCC in Makar

TinyCC is Makar's practical in-OS compiler. It runs as a normal ring-3 ELF
program (`/apps/tcc.elf`), reads headers and libraries from the shipped
sysroot, writes static ELF outputs, and lets the operator compile and run code
without leaving the OS.

This page replaces the old feasibility spike with a current-state reference.
The spike has shipped; the remaining questions are maintenance and expansion.

## Model

TCC is not a kernel component. It is a userspace program:

```text
shell -> exec /apps/tcc.elf -> read source -> write output ELF -> shell execs output
```

The workflow is deliberately simple:

```sh
tcc /usr/share/examples/hello-tcc.c -o /tmp/hello.elf
exec /tmp/hello.elf
```

This mirrors older small-system workflows: edit a file, compile it to a static
program, run it.

## Installed Files

The ISO stages a small sysroot:

| Path | Purpose |
|---|---|
| `/apps/tcc.elf` | TinyCC executable |
| `/usr/include/` | Makar userspace headers |
| `/usr/lib/crt0.o` | process entry object |
| `/usr/lib/libc.a` | userspace libc shim |
| `/usr/share/examples/hello-tcc.c` | example source |

TCC is configured to search these paths when compiling inside Makar.

## Runtime Dependencies

TCC relies on the userspace libc shim for:

- heap allocation: `malloc`, `free`, `realloc`, `calloc`
- file I/O: `open`, `read`, `write`, `close`, `lseek`, `unlink`
- buffered I/O: `fopen`, `fdopen`, `fread`, `fwrite`, `fprintf`, `fflush`
- string and memory functions
- formatting helpers
- `setjmp` / `longjmp`
- environment lookup
- path/cwd helpers
- archive and object writing support

These are implemented in `src/userspace/` and tested by `libc-tcc.sh`.

## Build Paths

There are two relevant build paths:

| Path | Purpose |
|---|---|
| host cross-build | builds `/apps/tcc.elf` as part of the image |
| in-guest TCC run | compiles examples/apps from inside the running OS |

The host build produces a static Makar ELF linked at the userspace load region.
The in-guest compiler then produces static ELF outputs that Makar's own
`elf_exec` can load.

## What TCC Can Do Today

The in-guest matrix covers:

- compile assembly stubs used by the kernel/userland examples
- compile and run `hello`
- compile apps such as `calc`, `sh`, `makbox`, `makmux`, `help`, `diskinfo`,
  `sigtest`, `forktest`, `execvetest`, `alloctest`, `filetest`
- compile larger UI/app programs such as `basic`, `fdisk`, `cfdisk`,
  `maktop`, `clock`, `lines`, `vix`, `kbtester`
- compile a freestanding source and link it manually
- build an object archive with `ar`
- link an executable from an object file and libc
- compile sources by relative path

Run the visible matrix with:

```sh
./run.sh gui libc
```

Or as part of the full visible guest suite:

```sh
./run.sh gui all-tests
```

## `tcc -run`

The supported workflow is compile-to-file then `exec`. `tcc -run` remains out
of scope as a supported feature.

Makar now has anonymous `mmap`, but the `-run` path expects a more complete
JIT-style execution environment: executable mappings, protection changes,
signal behavior, and loader semantics that are not part of the current design.
That can be revisited later, but it is not required for the CP/M-style in-OS
compiler goal.

## Floating Point

The kernel now initializes the x87 FPU and saves/restores x87/SSE state per
task. That removes the old system-level blocker for floating-point state
corruption across context switches.

The remaining limitation is library/tooling coverage:

- no shipped userspace `<math.h>`
- limited float formatting/parsing in the Makar libc shim
- TCC paths that fold or emit floating-point code still need targeted testing

Integer-only programs are the well-tested path.

## File Size and Storage Constraints

The practical constraints for in-OS compilation are still storage and memory:

- live ISO content is read-only
- `/tmp` is RAM-backed and good for temporary outputs
- installed writable filesystems are the right place for persistent outputs
- large source or output files stress the simple file-buffer model

Use `/tmp` for examples and installed ext2/FAT32 storage for persistent work.

## Why TinyCC Remains the Resident Compiler

Static musl support is being developed for broader hosted compatibility, but
TinyCC remains the resident compiler because it is:

- small
- understandable
- fast enough in the guest
- static-link friendly
- compatible with Makar's write-file-then-exec model

GCC self-hosting is a much larger project involving memory pressure, process
semantics, filesystem robustness, and likely architecture changes. TCC gives
Makar a useful self-hosted development loop now.

## Source Map

| File | Role |
|---|---|
| `vendor/tinycc/` | vendored TinyCC source |
| `build-tcc.sh` | host-side TCC build integration |
| `src/userspace/tcc_compat.c` | compatibility wrappers TCC needs |
| `src/userspace/libc-tcc.sh` | in-guest compile/run matrix |
| `src/userspace/Makefile` | builds apps, `libc.a`, headers, and sysroot staging |
| `src/userspace/hello-tcc.c` | canonical in-guest example |
| `toolchain/` | separate static-musl cross-toolchain scaffold |

## Maintenance Rules

When changing libc, syscalls, or TCC integration:

1. Run `./run.sh gui libc` or `./run.sh gui all-tests`.
2. Keep `src/userspace/Makefile` dependency generation working.
3. Avoid adding hidden host-only assumptions to the in-guest compile path.
4. Keep examples writing to `/tmp` or a writable root, not the ISO mount.
5. Update [Userland libc](userland-libc.md) and [POSIX compliance](posix.md)
   when a new libc surface becomes reliable.
