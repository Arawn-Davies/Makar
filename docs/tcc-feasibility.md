---
title: TCC in-OS — feasibility spike
nav_order: 5
---

# Porting TCC to run inside Makar — feasibility spike

**Status:** Phases 1 & 2 shipped (May 2026). The kernel-side file I/O
foundation and the userspace libc shim needed by TCC are both in tree
with ktest + ui_test coverage. Phase 3 (cross-build TCC against the
new sysroot) is the remaining milestone.

Goal: get TCC compiled as a cross-target, inventory exactly what it
needs to *run* on a live Makar system and compile/link other apps, and
lay out a concrete phased plan.

**Conclusion up front:** TCC the *compiler* is portable and i386 is a
first-class TCC target, so the compiler core is not the hard part. The hard
part is that **Makar has no hosted libc** — `src/userspace/` apps are
freestanding (`syscall.h` wrappers + the kernel's `libk.a` string/printf
subset). TCC needs `malloc/free/realloc`, buffered `stdio` (`FILE*`,
`fopen/fread/fwrite`), and real file create/write. Those don't exist in
userspace yet. The kernel-side syscall gaps are small and well-scoped; the
libc is the real work, and it's the same parked work the roadmap already
tracks (target: uClibc-ng, see `CLAUDE.roadmap.md` and `docs/userland-libc.md`).

---

## What TCC needs at runtime

TCC (mob/0.9.27 line) is ~100–200 KiB of C. As a *hosted* program it calls,
roughly:

| Category | Symbols TCC uses | Makar userspace today |
|---|---|---|
| Heap | `malloc free realloc calloc` | ❌ none (only raw `SYS_BRK`) |
| Buffered I/O | `fopen fdopen fclose fread fwrite fputs fprintf vfprintf fflush fseek ftell` | ❌ no `FILE*` layer |
| Raw file I/O | `open close read write lseek unlink` | ⚠️ partial — see gaps |
| String/mem | `memcpy memmove memset strcmp strncmp strcpy strncpy strcat strlen strchr strrchr strstr strdup` | ✅ in `libk.a` (except `strdup`) |
| Formatting | `snprintf vsnprintf sscanf` | ⚠️ `printf` exists; `snprintf`/`sscanf` missing |
| Control flow | `setjmp longjmp` | ❌ none |
| Misc | `qsort getenv atoi strtol strtod exit abort` + `<ctype.h>` | ❌ mostly missing |

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

## Makar gap analysis (verified against the tree)

### Kernel syscall gaps — small, well-scoped

1. **No file create / no `O_CREAT`/`O_TRUNC`.** `SYS_OPEN` (`syscall.c:542`)
   always `vfs_read_file()`s the path into a heap buffer and **fails for a
   nonexistent file**. TCC can't create its output object.
2. **`SYS_WRITE` on a `FD_KIND_FILE` returns −1** (`syscall.c:360`, "not
   writable through this fd today"). File writes today only go through
   `SYS_WRITE_FILE` (whole-buffer, overwrite) — `vfs_write_file`. No
   incremental write, no append-on-fd, no flush-on-close.
3. **64 KiB file cap.** `SYSCALL_FILE_MAX` (`syscall.h:89`) caps both the
   eager read buffer and the practical write size. Fine for `hello.c`, but a
   ceiling for real sources/outputs.
4. **No `SYS_STAT`/`SYS_FSTAT`.** TCC stats include files / output paths.
5. **No `SYS_READDIR`.** Only needed if TCC scans an include dir; usually it
   opens explicit header paths, so this is optional for v1.
6. **`SYS_BRK` exists** (`syscall.c:611`) and grows the user heap on demand —
   good enough to back a `malloc`.
7. **`fork`/`execve`/`wait4` exist** — so a future `tcc`-driven build script,
   or running the compiled output, works.

### Userspace libc gap — the real work

There is **no hosted libc**. `src/userspace/*.c` each roll their own buffers
and call `syscall.h` directly; `libk.a` provides only `string.*`, `memset`,
`printf`/`puts`/`putchar`, `abort`. Missing for TCC: a heap allocator, the
entire `FILE*`/`stdio` buffering layer, `snprintf`/`sscanf`, `setjmp`,
`<ctype.h>`, `qsort`, `getenv`, `strtol`/`strtod`, `strdup`.

Two ways to close it:

- **(A) Minimal hosted shim** (Makar-specific, ~1–2k LoC): `malloc` over
  `SYS_BRK`; a small `FILE*` over the fd syscalls; `snprintf`/`sscanf`;
  `setjmp.S`; `ctype`; `qsort`; stub `getenv`. Smallest path to *just TCC*,
  fully under our control, no porting friction.
- **(B) uClibc-ng static** (roadmap's stated target): a real, complete libc.
  More upfront porting (config for no-MMU/no-thread/static, wire its syscall
  layer to Makar's numbers) but pays off for every future app, not just TCC.

For a *spike → first working tcc*, (A) is the fastest credible route; (B) is
the right long-term investment. They're not mutually exclusive — a shim now
de-risks the compiler bring-up; uClibc-ng can replace it later behind the same
headers.

### ELF shape (loader constraints)

`elf.c:85` requires **`ET_EXEC`**, maps `PT_LOAD` segments, and expects them
**above `USER_CODE_BASE = 0x40000000`** (`crt0.S` + `link.ld`). TCC defaults
to the Linux i386 base `0x08048000` and emits a Linux-flavoured static exe.
So TCC-on-Makar must link target programs with an explicit base/text address
matching `link.ld` (e.g. `-Wl,-Ttext,0x40000000` or a Makar `link.ld` fed to
TCC's linker), `-static`, `-nostdlib`, against `crt0.o` + `libk.a`/`libc.a`.
This needs validation early — it's the most likely "compiles but won't load"
trap.

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
| TCC bring-up | Cross-build, ELF base = `USER_CODE_BASE`, sysroot header tree, in-OS `tcc hello.c -o hello.elf` | ⏭ Phase 3 |
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

**Phase 3 — cross-build `tcc.elf`. ⏭ next.** Link TCC against the
Phase-2 shim + `crt0.o` at `USER_CODE_BASE`; configure its target so
emitted programs are Makar-loadable `ET_EXEC` (`-Wl,-Ttext,0x40000000
-nostdlib -static`).  Ship `/usr/include` header tree on the OS image
so in-OS compiles can resolve headers, and ship the shim object files
(or pre-archive into `libc.a`) so emitted programs can link.

**Phase 4 — in-OS bring-up.** `tcc hello.c -o hello.elf` on a running
Makar, then `exec hello.elf`. Iterate on size limits, header coverage,
and self-host (building Makar userspace apps in-OS).

---

## Risks / open questions

- **ELF base/shape mismatch** (Phase 3) — most likely failure mode; validate
  the link recipe against `elf.c` early with a hand-linked stub.
- **64 KiB I/O ceiling** — TCC sources/outputs may exceed it; Phase 1 should
  raise or remove the cap, not just paper over it.
- **Heap pressure** — TCC holds the whole TU + symbol tables in RAM; the
  ring-3 `SYS_BRK` heap and kernel heap (`HEAP_MAX−HEAP_START` ≈ 16 MiB) must
  comfortably fit a real compile. Measure during Phase 0/3.
- **Shim vs uClibc-ng** — decision point at Phase 2 (see above); spike result
  (Phase 0 symbol set) should inform it.

---

## References

- `docs/userland-libc.md` — libc roadmap, sysroot layout, syscall status table.
- `CLAUDE.roadmap.md` §"In-kernel compiler" / "Userspace / libc porting".
- TCC: https://repo.or.cz/tinycc.git — i386 is a native target.
