---
title: TCC in-OS — feasibility spike (shipped)
parent: Development
nav_order: 2
---

# Porting TCC to run inside Makar — historical write-up

**Status (May 2026, v0.9):** all phases shipped.  TCC cross-builds cleanly
against the Makar libc shim, ships on every ISO image as `/apps/tcc.elf`,
**self-rebuilds calc.elf + sh.elf in-OS** (v0.8), **AND rebuilds the bootable
Multiboot 2 kernel ELF itself** under our build-kernel-tcc.sh driver (v0.9
host-side, in-OS in progress).  The sysroot (`/usr/lib/`, `/usr/include/`,
`/usr/lib/tcc/`, `/usr/include/kernel-build/`) is auto-staged by `build-tcc.sh`
+ `iso.sh`.

This page was originally a forward-looking feasibility spike; it now reads
as a historical write-up of how the path was scoped + landed.  For the
current state and the kernel-self-host work specifically, see
[rebuild-kernel.md](rebuild-kernel.md) and `CLAUDE.history.md`.

Goal at the time of writing: get TCC compiled as a cross-target, inventory
exactly what it needs to *run* on a live Makar system and compile/link other
apps, and lay out a concrete phased plan.

**Conclusion up front (still accurate):** TCC the *compiler* is portable and
i386 is a first-class TCC target, so the compiler core was not the hard
part.  The hard part was that Makar had no hosted libc — that gap was closed
by the freestanding shim in Phase 2.  Phases 3+ shipped: TCC cross-builds
against the shim, `tcc.elf` ships on every OS image alongside the full
sysroot (CRT objects, `libc.a`, `libtcc1.a`, headers), userspace apps
(calc/sh/makbox/hello) all self-rebuild end-to-end, and the **kernel** does
too.

---

## How TCC fits into Makar

TCC (Tiny C Compiler, `vendor/tinycc/`, v0.9.27) is being ported as an
**in-OS C compiler** — the end-state is a CP/M-style workflow where a user
can boot Makar, write a `.c` file in VIX, compile it with `tcc`, and run the
resulting ELF, all on bare metal.

### Architecture at a glance

```
 ┌─────────────────────── Makar (ring 0) ────────────────────────┐
 │  kernel: syscalls, VFS, FAT32/ext2/ISO 9660, scheduler, VMM  │
 └──────────────┬──────────────────────────────────┬─────────────┘
                │ int 0x80                         │
 ┌──────────────▼──────────┐  ┌────────────────────▼─────────────┐
 │  tcc.elf  (ring 3)      │  │  hello.elf  (ring 3)             │
 │  cross-built from       │  │  compiled by tcc.elf on a        │
 │  vendor/tinycc/ against │  │  running Makar system, linked    │
 │  the userspace libc     │  │  against crt0.o + libc.a at      │
 │  shim (libc.a)          │  │  /usr/lib, headers at            │
 │                         │  │  /usr/include                    │
 └─────────────────────────┘  └──────────────────────────────────┘
```

- **TCC runs in userspace (ring 3)**, not inside the kernel. It is a
  regular ELF binary loaded by `elf_exec()` and dispatched via the shell's
  `exec` command, like any other app (`calc.elf`, `vix.elf`, etc.).
- **No JIT (`tcc -run`)**: Makar has no `mmap(PROT_EXEC)`, so the
  in-memory JIT path is out of scope. TCC compiles to a static ELF file on
  disk, which is then `exec`'d from the shell.
- **Sysroot on the OS image**: `make install` (in `src/userspace/Makefile`)
  ships `crt0.o` and `libc.a` to `/usr/lib/`, headers to `/usr/include/`,
  and example sources to `/usr/share/examples/` on the ISO/HDD image.
  TCC's `CONFIG_TCC_SYSINCLUDEPATHS` and `CONFIG_TCC_LIBPATHS` are
  configured to point at these paths.
- **`build-tcc.sh`** is the cross-build script at the repo root. It
  compiles `vendor/tinycc/tcc.c` (with `ONE_SOURCE=1`) against the Makar
  sysroot using the `i686-elf-gcc` cross-compiler, then links
  `tcc.elf` at `USER_CODE_BASE = 0x40000000`.

### Key source files

| File | Role |
|---|---|
| `vendor/tinycc/` | Upstream TCC v0.9.27 snapshot (LGPL-2.1), unmodified except for `patches/` |
| `build-tcc.sh` | Cross-build script: probe-compile → link → `tcc.elf` |
| `src/userspace/tcc_compat.c` | POSIX-wrapper shim (`open`/`close`/`read`/`write`/`lseek`/`fseek`/`ftell`/`fdopen`/`strtoll`/`sprintf`/`exit`/`mmap` stub, etc.) — linked into `libc.a` |
| `src/userspace/hello-tcc.c` | Canonical test source shipped at `/usr/share/examples/hello-tcc.c` |
| `src/userspace/libc.a` | Archive of the freestanding libc shim (malloc, stdio, setjmp, string, tcc_compat) |
| `src/userspace/Makefile` | Builds `libc.a`, ships sysroot to `/usr/{lib,include}` on the image |
| `docs/tcc-feasibility.md` | This document |

---

## Compiling hello.elf inside Makar

> **Status: ready.** `tcc.elf` ships on every ISO at `/apps/`.
> The walkthrough below describes the workflow.

### The example source

A canonical test program ships on every Makar image at
`/usr/share/examples/hello-tcc.c`. It deliberately avoids the libc
shim's `stdio.h` so the very first in-OS compile doesn't need TCC to
resolve buffered-I/O headers — it talks directly to the kernel via
`syscall.h`:

```c
/* hello-tcc.c */
#include "syscall.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    sys_write(2, "Hello, TCC\n", 11);
    return 0;
}
```

`sys_write(2, ...)` writes to fd 2 (stderr), which prints to both the
VGA framebuffer **and** the serial port — ideal for both interactive use
and automated test assertions.

### Workflow

Boot Makar and reach the shell prompt. Then:

```
# 1. Compile — TCC reads hello-tcc.c, links against
#    /usr/lib/crt0.o + /usr/lib/libc.a, and writes hello.elf
#    to the current directory (writable rootfs, or /tmp ramdisk).
cd /tmp
tcc /usr/share/examples/hello-tcc.c -o hello.elf

# 2. Run the result.
exec hello.elf
```

Expected output on screen and serial:

```
Hello, TCC
```

### What happens under the hood

1. The shell resolves `tcc` to `/apps/tcc.elf` via the
   `PATH` variable and dispatches it through `elf_exec()`.
2. TCC runs in **ring 3** as a regular userspace task. It opens the
   source file via `SYS_OPEN`, reads it via `SYS_READ`, compiles the
   translation unit in RAM (heap via `SYS_BRK`), and writes the output
   ELF via `SYS_OPEN(O_CREAT|O_TRUNC)` + `SYS_WRITE` + `SYS_CLOSE`
   (the close flushes the dirty buffer to disk).
3. TCC's `CONFIG_TCC_SYSINCLUDEPATHS` is set to `/usr/include`, so
   `#include "syscall.h"` resolves to the shipped copy. Its
   `CONFIG_TCC_CRTPREFIX` and `CONFIG_TCC_LIBPATHS` point to `/usr/lib`,
   where `crt0.o` and `libc.a` live.
4. The emitted ELF is a **static `ET_EXEC`** linked at
   `USER_CODE_BASE = 0x40000000` — the same base address as every other
   Makar userspace binary. Makar's `elf_exec()` loads it, maps a fresh
   user page directory, and enters ring 3.
5. `hello.elf` calls `sys_write(2, "Hello, TCC\n", 11)` via `int 0x80`,
   the kernel writes to VGA + serial, then `main` returns 0 and `crt0`
   issues `SYS_EXIT(0)`.

### Writing your own programs

You can also write a source file from within Makar using the VIX editor,
then compile and run it — the full CP/M-style edit → compile → run loop:

```
# 1. Write a new source file on the writable rootfs (or /tmp).
cd /tmp
vix myapp.c

# 2. Compile it (headers at /usr/include, libs at /usr/lib).
tcc myapp.c -o myapp.elf

# 3. Run it.
exec myapp.elf
```

Programs that include `<stdio.h>` (for `printf`, `fopen`, etc.) or
`<stdlib.h>` (for `malloc`, `atoi`, etc.) will resolve those headers
from `/usr/include` and link against `libc.a` at `/usr/lib`
automatically. The libc shim provides:

- **Heap**: `malloc` / `free` / `realloc` / `calloc` (over `SYS_BRK`)
- **Buffered I/O**: `fopen` / `fread` / `fwrite` / `fclose` / `fprintf` / `printf`
- **Strings**: `strlen` / `strcmp` / `strcpy` / `strdup` / `memcpy` / `memset`
- **Misc**: `atoi` / `strtol` / `qsort` / `setjmp` / `longjmp`

### Limitations

- **No `tcc -run`** — Makar has no `mmap(PROT_EXEC)`, so TCC cannot
  JIT-execute in memory. You must compile to a file and `exec` it.
- **No floating point** — the kernel doesn't initialise the x87 FPU, so
  `float` / `double` literals will misfold. Integer-only programs work.
- **8 MiB file cap** — `SYSCALL_FILE_MAX` limits any single file
  (source or output) to 8 MiB.
- **Output must go to writable storage** — the ISO 9660 CD-ROM
  (`/mnt/cdrom`) is read-only. Write output files to the writable rootfs
  (ext2/FAT32 if installed) or to `/tmp` (in-RAM ramdisk).

---

## What TCC needs at runtime

TCC (mob/0.9.27 line) is ~100–200 KiB of C. As a *hosted* program it calls,
roughly:

| Category | Symbols TCC uses | Makar userspace status |
|---|---|---|
| Heap | `malloc free realloc calloc` | ✅ `malloc.{h,c}` over `SYS_BRK` |
| Buffered I/O | `fopen fdopen fclose fread fwrite fputs fprintf vfprintf fflush fseek ftell` | ✅ `stdio.{h,c}` + `tcc_compat.c` |
| Raw file I/O | `open close read write lseek unlink` | ✅ `tcc_compat.c` wrappers over syscalls |
| String/mem | `memcpy memmove memset strcmp strncmp strcpy strncpy strcat strlen strchr strrchr strstr strdup` | ✅ `libk.a` + `strdup` in `stdlib.h` |
| Formatting | `snprintf vsnprintf sscanf sprintf vsprintf` | ✅ `stdio.{h,c}` + `tcc_compat.c` |
| Control flow | `setjmp longjmp` | ✅ `setjmp.{h,S}` |
| Misc | `qsort getenv atoi strtol strtoll strtod exit abort` + `<ctype.h>` | ✅ `stdlib.h` + `ctype.h` + `tcc_compat.c` |
| Stubs (JIT path) | `mmap munmap mprotect` + signal types | ✅ stub-only (return `MAP_FAILED`/`-1`); JIT path unreachable |

For **emitting** a program, TCC also needs to:
1. **Create and write an output file** (`fopen(out,"wb")` → many `fwrite`s).
2. Emit a **static ET_EXEC ELF32** whose load address and shape match Makar's
   loader (see "ELF shape" below).
3. Find **system headers** (`#include <...>`) and a **crt + libc archive** to
   link the target program against.

`tcc -run` (in-memory JIT execute) additionally needs executable memory
(`mmap`/`mprotect` with `PROT_EXEC`). Makar has neither, and ring-3 code runs
out of the kernel ELF loader's mappings — so **`-run` is out of scope**; the
realistic model is *compile to an ELF file, then `exec` it from the shell*
(CP/M / ELKS style).

---

## Makar gap analysis — what's left for Phase 3

### Kernel syscall layer — ✅ complete

All syscalls TCC needs are in place: `SYS_OPEN` with `O_CREAT`/`O_TRUNC`/
`O_APPEND`, writable `FD_KIND_FILE` with krealloc grow and flush-on-close,
`SYS_STAT`/`SYS_FSTAT`, `SYS_READDIR`, `SYS_BRK`, `SYS_LSEEK`,
`fork`/`execve`/`wait4`. `SYSCALL_FILE_MAX` is 8 MiB.

### Userspace libc shim — ✅ complete

The freestanding shim in `src/userspace/` covers every symbol TCC
references: heap (`malloc.{h,c}`), `FILE*` I/O (`stdio.{h,c}`),
`setjmp`/`longjmp` (`setjmp.{h,S}`), `<ctype.h>`, `strtol`/`atoi`/
`strdup`/`qsort`/`sscanf`/`getenv`, and the POSIX wrappers in
`tcc_compat.c` (`open`/`close`/`read`/`write`/`lseek`/`fseek`/`ftell`/
`fdopen`/`sprintf`/`strtoll`/`exit`/`abort`/`mmap` stub/`getcwd`/etc.).

### TCC source porting — ✅ complete

`build-tcc.sh` produces a clean `tcc.o` and links `tcc.elf` at
`USER_CODE_BASE = 0x40000000`. The concrete approach:

| TCC source file | What it pulls in | Fix applied |
|---|---|---|
| `tccrun.c` | JIT path (`-run`) | Already guarded by `#ifdef TCC_IS_NATIVE`; `CONFIG_TCCBOOT` prevents `TCC_IS_NATIVE` from being defined |
| `tccpp.c` | `<time.h>` (`struct tm`, `localtime`) | Stub `localtime()` in `tcc_compat.c`; stub `time.h` in build-stubs |
| `libtcc.c` | `fdopen`, `fseek`, `ftell`, `exit`, `strtoll` | ✅ All in `tcc_compat.c` |
| `tccelf.c` | `ssize_t` | ✅ Declared in stub `stdint.h` |
| `i386-link.c` | `ELF_START_ADDR` defaulting to `0x08048000` | Patched to `0x40000000` under `CONFIG_TCCBOOT` (patch `001-makar-elf-start-addr.patch`) |

The build also produces:
- `libtcc1.a` (TCC's runtime library for compiled programs, providing 64-bit arithmetic helpers)
- CRT stubs (`crt1.o` = copy of `crt0.o`, empty `crti.o`/`crtn.o`) so TCC's default link path works without `-nostdlib`

---

## Status at-a-glance (May 2026)

| Layer | Surface | Status |
|---|---|---|
| Kernel | `O_CREAT/O_TRUNC/O_APPEND`, writable `FD_KIND_FILE` (krealloc grow), flush-on-close, `SYS_STAT/FSTAT`, `vfs_stat`, `ext2_stat`, `SYS_READDIR(141)` + `struct dirent` | ✅ Phase 1 (+ readdir slice) |
| libc heap | `malloc`/`free`/`realloc`/`calloc` over `SYS_BRK` (`src/userspace/malloc.[ch]`) | ✅ Phase 2a |
| libc strings | `<ctype.h>`, `strdup`, `strtol`/`atoi`, `sscanf` (`src/userspace/{ctype,stdlib}.h`) | ✅ Phase 2a |
| libc control | `setjmp`/`longjmp` (`src/userspace/setjmp.{h,S}`) | ✅ Phase 2a |
| libc stdio | `FILE*` + `fopen`/`fread`/`fwrite`/`fclose`/`fputs`/`fputc`/`fgetc`/`fflush`, `snprintf`/`vsnprintf`/`fprintf`/`printf` (`src/userspace/stdio.[ch]`) | ✅ Phase 2b |
| libc misc | `qsort`, `getenv` (stub) | ✅ Phase 2c |
| Coverage | `filetest.elf`, `alloctest.elf` (12 sub-tests + 8 ktests in `test_file_fd`) | ✅ |
| TCC bring-up | Cross-build, ELF base = `USER_CODE_BASE`, sysroot header tree, `tcc.elf` on ISO | ✅ Phase 3 |
| Follow-ups | Move bootfs off FAT32 to enable <33 MiB (limine BIOS is FAT32/ISO9660-only today — would need a FAT12/16-capable bootloader, not a Phase-3 dependency); refcounted `open_file_t` to make fork-shared file offsets POSIX-correct | ⏭ |

## Phased plan

**Phase 0 — spike (this doc), + confirm the build. ✅ done.** Cross-build TCC in the
existing Docker toolchain (`arawn780/gcc-cross-i686-elf:fast`) as a sanity
check that the i386 backend targets our triple, and capture its exact
undefined-symbol set (`i686-elf-nm`/link errors) to pin the libc surface
empirically rather than from this table. *No kernel changes.*

**Phase 1 — kernel file I/O (own PR). ✅ shipped.**
- `SYS_OPEN` honours `O_CREAT`/`O_TRUNC`/`O_WRONLY`/`O_APPEND`; growable
  `FD_KIND_FILE` buffer via `krealloc` doubling; close flushes via
  `vfs_write_file` and propagates backend errors.
- `SYS_LSEEK` past EOF on writable fds; next `SYS_WRITE` zero-fills the gap.
- `SYS_STAT(106)`/`SYS_FSTAT(108)` populate Linux i386 `struct stat`;
  `st_ino` is FNV-1a-32 of the resolved path.
- `vfs_stat` dispatches to `ext2_stat` (new) and `fat32_read_file(NULL,…)`
  / `iso9660_read_file(NULL,…)` for lean size probes; falls back to
  scratch-read on `procfs`/`logfs`.
- `SYSCALL_FILE_MAX` lifted from 64 KiB → 8 MiB.
- `fd_table_clone` (fork) deep-copies the FILE buffer but clears `dirty`
  on the child copy so only the parent flushes — pragmatic non-POSIX
  shortcut until the `open_file_t` refactor lands.
- Coverage: `test_file_fd` ktest suite (8 sub-tests) + `filetest.elf`
  ui scenario.

**Phase 2 — hosted libc shim. ✅ shipped.**
Path (A) -- Makar-specific shim, header-only where possible.  Lives
under `src/userspace/`:

| File | Provides |
|---|---|
| `malloc.{h,c}` | First-fit free-list over `SYS_BRK`, address-sorted coalescing |
| `ctype.h` | `is*`/`tolower`/`toupper` ASCII inlines |
| `stdlib.h` | `strtol`/`atoi`/`strdup`/`qsort`/`sscanf`/`getenv` (stub) |
| `setjmp.{h,S}` | i386 SysV `jmp_buf[6]` (ebx/esi/edi/ebp/esp/eip) |
| `stdio.{h,c}` | `FILE*` + `fopen`/`fread`/`fwrite`/`fclose`/`fputs`/`fputc`/`fgetc`/`fflush`, `snprintf`/`vsnprintf`/`fprintf`/`printf` |

Validated end-to-end by `alloctest.elf` (12 sub-tests: heap reuse,
realloc grow, calloc zero, ctype, strtol, atoi, setjmp/longjmp +
POSIX 0→1 quirk, snprintf format + truncation, FILE\* roundtrip,
`sys_readdir`, strdup/qsort/sscanf/getenv).

Path (B) -- uClibc-ng static -- still the long-term direction once
the kernel-side `SYS_PIPE`/`SYS_DUP2`/`SYS_MMAP(MAP_ANONYMOUS)` gaps
close.  The shim above can be replaced behind the same headers when
that lands.

**Phase 3 — cross-build `tcc.elf`. ✅ shipped.** `build-tcc.sh` compiles
`vendor/tinycc/tcc.c` (with `ONE_SOURCE=1`) against the Phase-2 shim using
`i686-elf-gcc`, links at `USER_CODE_BASE = 0x40000000`, and ships `tcc.elf`
on the ISO. Also builds `libtcc1.a` (64-bit arithmetic runtime) and CRT
stubs (`crt1.o`/`crti.o`/`crtn.o`). The ELF start address is patched to
`0x40000000` via `patches/001-makar-elf-start-addr.patch`. TCC's
`CONFIG_TCC_SYSINCLUDEPATHS` resolves `/usr/include` (libc) and
`{B}/include` (TCC builtins: `stdarg.h`, `stddef.h`, etc.).

**Phase 4 — in-OS bring-up.** `tcc hello.c -o hello.elf` on a running
Makar, then `exec hello.elf`. Iterate on size limits, header coverage,
and self-host (building Makar userspace apps in-OS).

---

## Risks / open questions

- **ELF base/shape** — ✅ resolved. TCC's i386 default (`0x08048000`) is
  patched to `0x40000000` via `patches/001-makar-elf-start-addr.patch`,
  gated on `CONFIG_TCCBOOT`. Programs compiled by TCC in-OS will emit
  `ET_EXEC` at the correct base address automatically.
- **Heap pressure** — TCC holds the whole TU + symbol tables in RAM; the
  ring-3 `SYS_BRK` heap and kernel heap (`HEAP_MAX−HEAP_START` ≈ 16 MiB) must
  comfortably fit a real compile. Measure during Phase 3.
- **No floating point in the kernel** — `CR0.MP` is not set and there's no
  `#NM` handler, so TCC's float-constant lexer paths (`strtod`/`strtof`/
  `strtold`) are stubbed to return 0. Integer-only sources work; programs
  with float literals will misfold until x87 FPU init lands.

---

## References

- `docs/userland-libc.md` — libc roadmap, sysroot layout, syscall status table.
- `CLAUDE.roadmap.md` §"In-kernel compiler" / "Userspace / libc porting".
- TCC: https://repo.or.cz/tinycc.git — i386 is a native target.
