# Full self-hosting: porting musl + a real toolchain to run *on* Makar

What would it take to build hosted C programs **on Makar itself** — eventually
running GCC + musl in-OS, not just cross-compiling for Makar from a dev box?

Honest answer up front: **a lot, in three very different tiers.** Tier 1 is
bounded and worth doing. Tier 3 (GCC on Makar) is a different universe and
needs the kernel to grow up substantially. The pragmatic self-host story stays
**TCC** (already working) for a long time.

---

## Two goals people conflate

1. **Run cross-built static-musl binaries on Makar.** This is what `toolchain/`
   + the B work (`mmap`, auxv, startup stubs, and the pending TLS gate) chase.
   Bounded: ~a dozen Linux syscalls + TLS and a stock `hello` runs.
2. **Run the *toolchain* on Makar** — compile + link hosted programs in-OS
   against a real libc. This page is about #2. It's the "awful lot."

---

## Where we are today

- **TCC** compiles C → ELF in-OS and self-rebuilds our apps *and the kernel*.
  That is the real, working self-host path. It needs no fork-of-GCC, no
  dynamic linking, no threads — that's why it works on a 32 MiB kernel.
- A **freestanding `libc.a` shim** (our own), not a real libc.
- Kernel: Linux i386 `int 0x80` ABI **subset**; `fork`+`execve`+`wait4` (COW);
  32 MiB heap; 256 MiB identity map; anon `mmap`; armed x87 with per-task
  save/restore. **No** TLS (pending), futex, demand paging, swap, dynamic
  linker, threads, or file-backed `mmap`.

---

## Tier 1 — musl as the system libc (bounded; the right next big rock)

Goal: programs (and eventually parts of the toolchain) static-link against
**musl** in-OS instead of our shim. musl is ~10× cleaner to port than glibc and
needs no kernel threads for static linking.

What the kernel must gain beyond the B prerequisites:

- **TLS** (`set_thread_area` + per-task `%gs`) — B's pending gate. Hard but
  one-time. Required by `__init_libc`.
- **`futex`** (`SYS_futex 240`) — musl's locks (`malloc`, stdio, `__lock`) call
  it. Single-threaded there is never contention, so a **no-op futex returning
  0** is enough. Cheap.
- **Broader syscall surface musl wraps** (most are thin shims over what we have
  or easy additions): `writev`/`readv`, `pread64`/`pwrite64`, `fcntl64`,
  `rt_sigaction`/`rt_sigprocmask`, `tgkill`/`tkill` (for `raise`/`abort`),
  `gettid`, `set_robust_list` (stub), `clock_nanosleep`/`nanosleep`,
  `getrandom` (or fall back to `/dev/urandom`), `madvise` (stub),
  `sched_getaffinity` (stub → 1 CPU), `uname`, `ppoll`/`poll` (stdio rarely),
  `mmap` file-backed (musl `fopen`+`mmap` paths — can route through our buffered
  files initially). Most are <20 lines each or a `return 0`/`-ENOSYS` stub.
- **`brk` + anon `mmap`** — have both; musl's mallocng uses `mmap`.
- **errno discipline**: musl reads `-errno` in `[-4096,-1]` from `eax`. Our
  legacy syscalls return `-1`; the musl-facing ones must return real `-errno`.
  (The `30a-30e` wrappers already set errno; the kernel side needs the negative
  return convention for these calls.)

Then: build musl itself. Options — (a) cross-build musl headers+`libc.a` with
our `toolchain/` and **ship it to `/usr/lib`** so in-OS programs link it; or
(b) compile musl's source **in-OS with TCC** (musl is C89-ish; TCC can build
most of it, though some asm/atomics need attention). (a) is far easier first.

**Verdict:** Tier 1 is a real but *finite* project — call it the TLS gate +
~30 syscall stubs/impls + an errno-convention pass + staging musl. After it, a
large class of ordinary hosted C software static-links and runs in-OS. **This
is the recommended direction after B.**

---

## Tier 2 — binutils (`as`, `ld`) on Makar

GCC's driver shells out to a real assembler and linker. To run GCC in-OS you
also need **GNU as + ld** running in-OS (or accept TCC's integrated codegen,
which means you're not running GCC). Porting binutils:

- Needs Tier-1 musl (they're hosted C programs) + lots of file I/O + meaningful
  RAM (ld linking can use 100s of MB).
- They `fork`/`exec` little themselves, so the process model is OK — it's the
  **memory and libc surface** that bites.
- Bounded but large; gated entirely on Tier 1.

---

## Tier 3 — GCC on Makar (the "awful lot")

This is where the kernel has to grow up. GCC compiling one non-trivial file:

- **Memory**: `cc1` routinely uses **100 MiB – 1 GiB**. Makar identity-maps
  256 MiB and a ring-3 process is capped well below that. This needs: a much
  larger user address space, **demand paging** (don't map eagerly), likely
  **swap**, and realistically **x86-64** (4 GiB+ address space) — a 32-bit
  `cc1` constantly fights the 3 GiB user limit on big translation units.
- **GCC's build deps**: `gmp`, `mpfr`, `mpc` (bignum + arbitrary-precision
  float). GMP/MPC are integer; **MPFR** is software arbitrary-precision float
  (doesn't strictly need the x87) but is large. All three are hosted C libs →
  gated on Tier 1.
- **Process model**: the GCC *driver* forks `cc1`/`as`/`ld`. We have
  `fork`+`exec`+`wait4`, but running 3–4 large processes concurrently in tens of
  MiB is not viable — see memory above.
- **Floating point + `<math.h>`**: libgcc and parts of the build want real FP.
  x87 is now armed + saved; a libc `<math.h>` still needs writing/porting.
- **Filesystem**: GCC writes lots of temp files (`/tmp`), reads a big sysroot.
  Our tmpfs is 16 files × 512 KiB — far too small; needs a real writable rootfs
  with space.
- **Build time**: compiling GCC itself is hours on fast hardware; under TCG with
  a 32-bit hobby kernel it's impractical without a lot of the above first.

**Verdict:** GCC-on-Makar is a long-horizon aspiration, not a port you "just
do." It effectively requires: Tier 1 + Tier 2 + demand paging + much more RAM +
probably an **x86-64 kernel** + a real writable FS + a `<math.h>`. Each of those
is itself a project. This is the "awful lot" — correctly intuited.

---

## Recommended path (most value per unit pain)

1. **Finish B** — TLS gate + `writev`; prove a cross-built static-musl `hello`
   runs. **Done** (`feat/dynamic-libc` Phase 0): TLS + futex were already in;
   `SYS_WRITEV` (146) was the missing piece and is now implemented. A static-musl
   `hello` runs end-to-end in-OS (`/apps/muslhello.elf`, gated by
   `shell-smoke.sh: musl-static`). Next: **dynamic** linking (musl `libc.so` +
   `ld-musl-i386.so.1`) — see `~/.claude/plans/melodic-honking-river.md`.
2. **Tier 1: musl as the system libc** — futex no-op, the ~30 syscalls, errno
   convention, ship musl to `/usr/lib`. Unlocks running a *lot* of real hosted
   software in-OS. **This is the high-value milestone.**
3. **Keep TCC as the in-OS compiler.** It already self-hosts the kernel; pair it
   with musl and you can build serious C in-OS without GCC.
4. **Tier 2 (binutils) only if a specific need demands GNU `as`/`ld`.**
5. **Tier 3 (GCC) / x86-64** — track as long-term direction. The honest move
   toward "GCC on Makar" is **start an `arch/x86_64/` kernel + demand paging**
   first; the userspace (VFS, shell, libc) ports with minimal changes.

Bottom line: **TCC + a ported musl is the realistic "fully self-hosting hosted
toolchain" for Makar.** Running GCC itself is a 64-bit, demand-paged,
much-bigger-RAM future — worth naming, not worth starting before Tier 1.
