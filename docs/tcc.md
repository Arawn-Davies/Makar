---
title: TinyCC in Makar
parent: Reference
nav_order: 6
---

# TinyCC in Makar

TinyCC is Makar's resident C compiler. It ships as `/apps/tcc.elf`, runs as a
normal ring-3 process, uses the Makar userspace sysroot, and writes static ELF
programs that the Makar loader can execute.

This is shipped functionality. The current model is deliberately simple: edit
source, compile to an ELF file, then execute that file.

```sh
tcc /usr/share/examples/hello-tcc.c -o /tmp/hello.elf
exec /tmp/hello.elf
```

## Design Contract

TinyCC is kept outside the kernel. The compiler depends on the same process,
filesystem, fd-table, libc, and ELF-loader paths that other userspace programs
use.

```text
sh.elf
  -> exec /apps/tcc.elf
       -> open/read source and headers
       -> allocate compiler state
       -> write object/archive/executable output
  -> exec compiled output
```

That contract matters because it makes TCC a systems test as much as a tool.
When TCC works in-guest, the OS has proven a useful combination of:

- process startup (`crt0.o`, argc/argv/envp, auxv)
- userspace allocation
- file opens, reads, writes, seeks, and unlinks
- relative and absolute path handling
- libc string, stdio, stdlib, setjmp, and formatting support
- archive and object output
- static ELF loading through `execve`

## Installed Layout

The image stages a small compiler sysroot:

| Path | Purpose |
|---|---|
| `/apps/tcc.elf` | TinyCC executable. |
| `/usr/include/` | Makar userspace headers. |
| `/usr/lib/crt0.o` | Makar process entry object. |
| `/usr/lib/libc.a` | Makar userspace libc shim. |
| `/usr/lib/tcc/` | TCC support files where packaged. |
| `/usr/share/examples/hello-tcc.c` | Minimal in-guest compile example. |
| `/src/` | Source tree staged for rebuild and compiler tests. |

The root filesystem layout intentionally resembles Unix paths because TinyCC,
future libc ports, and shell scripts all become simpler when `/usr/include`,
`/usr/lib`, `/apps`, `/tmp`, and `/src` mean what hosted tools expect.

## What Works

The supported path is compile-to-file:

```sh
tcc source.c -o /tmp/program.elf
exec /tmp/program.elf
```

The in-guest TCC/libc matrix covers:

- compiling and running small C programs
- compiling sources from relative and absolute paths
- compiling assembly inputs used by the kernel/userspace support files
- creating object files
- archiving objects
- linking an executable from objects plus `libc.a`
- rebuilding userspace apps such as `calc`, `sh`, `makbox`, `makmux`, `help`,
  `diskinfo`, `sigtest`, `forktest`, `execvetest`, `alloctest`, and `filetest`
- compiling larger interactive programs such as `basic`, `fdisk`, `cfdisk`,
  `maktop`, `clock`, `lines`, `vix`, and `kbtester`

Run that matrix with:

```sh
./run.sh gui libc
```

or as part of the visible in-guest suite:

```sh
./run.sh gui all-tests
```

The headless CI path runs the same in-guest script driver through the
test-mode boot flow.

## Kernel Rebuild

Makar also uses TinyCC for the kernel self-hosting path. There are two related
but distinct workflows:

| Workflow | Where it runs | Purpose |
|---|---|---|
| `./build-kernel-tcc.sh` | host | Build a Multiboot 2 kernel ELF with the vendored TinyCC toolchain. |
| `/apps/rebuild-kernel.sh` | inside Makar | Run the same style of kernel rebuild from the running OS. |

The kernel rebuild path is documented in [Rebuild kernel](rebuild-kernel.md).
This page focuses on `/apps/tcc.elf` as the resident compiler and the sysroot it
uses.

## Runtime Dependencies

TCC exercises a broad userspace surface. The most important dependencies are:

| Area | Required behavior |
|---|---|
| Process model | `execve`, `wait4`, exit status propagation, stable argv/envp startup. |
| Files | `open`, `read`, `write`, `close`, `lseek`, `unlink`, relative paths, cwd. |
| FDs | duplicated descriptors, stderr diagnostics, buffered stdio over fds. |
| Memory | `brk`/malloc and anonymous `mmap` support where libc paths use it. |
| Libc | string/memory routines, `stdio`, `stdlib`, `ctype`, `errno`, `setjmp`. |
| Time/path helpers | enough hosted-style support for TCC diagnostics and output paths. |
| ELF | static executable output compatible with Makar's loader. |

Most compatibility glue lives in:

- `src/userspace/tcc_compat.c`
- `src/userspace/stdio.c`
- `src/userspace/stdlib.c`
- `src/userspace/syscall.h`
- `src/userspace/Makefile`

## Intentional Limits

### `tcc -run`

`tcc -run` is not the supported execution model. Makar supports compiling to an
ELF file and executing the file.

`tcc -run` expects a more complete hosted/JIT-style environment: executable
anonymous mappings, protection changes, loader behavior for memory-resident
code, and richer signal semantics. Makar intentionally avoids that for now.

### Floating Point

The kernel initializes x87/SSE state and saves/restores it per task, so the old
system-level FPU blocker is gone.

The remaining gaps are libc/tooling gaps:

- no complete shipped userspace `<math.h>`
- limited floating-point formatting and parsing
- limited test coverage for TCC paths that fold or emit floating-point code

Integer C programs are the tested path.

### Storage and Memory

Use `/tmp` for temporary compiler output. Use an installed writable FAT32/ext2
filesystem for persistent work. ISO contents are read-only.

Large compilations still stress the simple file and memory model. TCC is small
enough to be useful today; GCC-scale self-hosting is a separate project with a
different memory, filesystem, and process-management bar.

## TinyCC vs Static musl

TinyCC and static musl solve different problems.

TinyCC is the resident compiler: small, fast enough in the guest, and compatible
with Makar's static write-file-then-exec workflow.

Static musl is the compatibility target for running outside programs. Its
startup path drives ABI work such as auxv, TLS, anonymous `mmap`, errno,
signals, and POSIX-shaped fd behavior. Makar can improve musl compatibility
without replacing TinyCC as the resident compiler.

See [Userland libc](userland-libc.md) and [POSIX compatibility](posix.md).

## Source Map

| File or directory | Role |
|---|---|
| `vendor/tinycc/` | Vendored TinyCC source. |
| `build-tcc.sh` | Host-side TCC build integration. |
| `build-kernel-tcc.sh` | Host-side kernel build through TinyCC. |
| `src/userspace/tcc_compat.c` | Hosted compatibility wrappers used by TCC. |
| `src/userspace/libc-tcc.sh` | In-guest TCC/libc regression matrix. |
| `src/userspace/Makefile` | Builds apps, `libc.a`, headers, sysroot staging, and dependency files. |
| `src/userspace/hello-tcc.c` | Canonical tiny compile example. |
| `src/userspace/rebuild-kernel.sh` | In-OS kernel rebuild script. |
| `toolchain/` | Separate static-musl cross-toolchain work. |

## Maintenance Rules

When changing syscalls, libc, the userspace Makefile, or TCC packaging:

1. Run `./run.sh gui libc` for the TCC/libc matrix.
2. Run `./run.sh gui all-tests` when the change also touches process, shell,
   fd, or filesystem behavior.
3. Keep `src/userspace/Makefile` dependency tracking intact; stale objects can
   make compiler bugs look like kernel bugs.
4. Keep examples and tests writing to `/tmp` or another writable filesystem.
5. Update this page, [Userland libc](userland-libc.md), and
   [POSIX compatibility](posix.md) when a new hosted surface becomes reliable.
