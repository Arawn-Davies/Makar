# Makar hosted toolchain

A **host-side cross toolchain** that targets Makar: `gcc` + `musl` + `binutils`
built as `i686-linux-musl-*`, producing **static musl i386 ELFs**. This is the
"hosted toolchain" — a real libc (musl) and a real C compiler (GCC), as opposed
to the freestanding `i686-elf-gcc` used for the kernel and the minimal
`libc.a` shim used for the in-tree ring-3 apps.

This is **separate** from the kernel build. Nothing here is required to build or
boot Makar; it's the path toward compiling ordinary hosted C programs (and
eventually porting `dash`, `coreutils`, etc.) for Makar.

## Why not GCC running *on* Makar?

Self-hosting GCC in-OS needs `mmap`, `fork`, ~100 Linux syscalls, and real RAM.
That's out of reach near-term. The in-OS self-host story stays **TCC** (`tcc.elf`),
which already rebuilds our apps and the kernel. This folder is about a **cross**
toolchain on the dev machine.

## What this produces

```
toolchain/install/bin/i686-linux-musl-gcc      # the cross compiler
toolchain/install/bin/i686-linux-musl-{ld,as,ar,...}
toolchain/install/i686-linux-musl/             # sysroot: musl libc + headers
```

`i686-linux-musl-gcc -static -march=i686 hello.c -o hello` emits a static
i386 ELF linked against musl, using the Linux i386 `int 0x80` ABI.

**Cross-platform invocation:** the installed compiler is a Linux x86-64 binary,
so on macOS/Windows run it through the container with the wrapper:

```sh
./toolchain/cc.sh -static -march=i686 hello.c -o hello   # mounts $PWD at /src
```

Verified: GCC 9.4.0 builds a `#include <stdio.h>` printf hello into a
statically-linked Intel 80386 ELF.

## Build

```sh
./toolchain/build-musl-cross.sh        # ~30-60 min; clones musl-cross-make, builds, installs
```

**Docker-first** (cross-platform, same strategy as `run.sh`): the script builds
a small `makar-toolchain:local` image (`toolchain/Dockerfile` — Debian + build
deps) and runs musl-cross-make inside it, writing the toolchain to the mounted
`toolchain/install/`. If `/.dockerenv` is already present it builds directly; if
Docker is absent it falls back to host `gcc`/`make`/`git`. musl-cross-make builds
its own `gmp`/`mpfr`/`mpc`, so no host math-lib dev packages are needed.

## The gap: making Makar *run* musl binaries

Building the toolchain is the easy half. A stock static-musl ELF will **not**
start on Makar yet. Ordered by what musl touches at process start:

1. **ELF auxv on the initial stack.** musl's `__init_libc` walks `argc, argv,
   envp, auxv[]` laid out on the stack at `_start`. Makar's `elf_exec` sets up
   `argc`/`argv` but **not** the `auxv` (`AT_PAGESZ`, `AT_PHDR/PHENT/PHNUM`,
   `AT_RANDOM` (16 bytes — musl reads it for stack-guard/TLS canary), `AT_NULL`).
   → extend the ELF loader's stack setup.
2. **TLS / thread pointer.** musl sets up TLS very early via
   `SYS_set_thread_area(243)` (i386 `set_thread_area` / `struct user_desc`),
   which installs a GDT entry and loads `%gs`. Makar has no TLS today.
   → add `set_thread_area`: allocate a GDT slot, point `%gs` base at musl's TLS
     block. (i386 musl reads the thread pointer through `%gs:0`.)
3. **`mmap(MAP_ANONYMOUS)`.** musl's malloc (mallocng/oldmalloc) gets memory via
   `mmap`, not `brk`. Makar's `mmap` is a stub returning `MAP_FAILED`.
   → implement anonymous `mmap`/`munmap` (map fresh frames into the task PD;
     a simple bump-or-freelist region above the heap is enough to start).
4. **A handful of startup syscalls** musl calls and currently aborts without:
   `set_tid_address(258)`, `rt_sigprocmask(175)`, `ioctl(54)` (for `isatty`),
   `writev(146)`, `readv(145)`, `nanosleep(162)`, `clock_gettime(265)` (have).
   → stub/implement; several can be thin shims over existing handlers.
5. **`%f` / `strtod` / `<math.h>` → x87.** A real libc does floating point.
   Makar must `fninit` the FPU and save/restore x87 state across context
   switches. See `docs/` and the kernel `fpu_init()` work (in progress).

Recommended order: **x87 init → ELF auxv → mmap(anon) → set_thread_area → the
syscall stubs → first `hello` runs.** Each is independently testable.

## Lighter alternative: newlib

If musl's TLS/mmap startup cost is too steep to start with, **newlib** is the
classic "retarget to a custom OS" libc: you implement ~17 syscall stubs
(`_read`, `_write`, `_open`, `_close`, `_sbrk`, `_fstat`, `_exit`, ...) that map
almost 1:1 onto Makar's existing syscalls — **no TLS, no mmap** needed for
static linking. It reaches a working hosted libc with far less kernel work, at
the cost of being older/less clean than musl. Kept as the fallback if the musl
bring-up stalls.

## Status

- [x] toolchain built (`build-musl-cross.sh`) — `i686-linux-musl-gcc` 9.4.0; static i386 musl ELFs verified
- [x] x87 `fpu_init()` in kernel (FPU armed at boot + `test_fpu` ktest) — per-task `fxsave`/`fxrstor` across context switches still TODO
- [ ] ELF loader emits auxv
- [x] `mmap(MAP_ANONYMOUS)` + `munmap` (`SYS_MMAP2`/`SYS_MUNMAP`; anon-only bump window, validated by alloctest #19)
- [ ] `set_thread_area` + `%gs` TLS
- [ ] startup syscall stubs
- [ ] first static-musl `hello` runs on Makar
