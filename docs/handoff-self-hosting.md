# Handoff — kernel self-host with TCC (v0.9.0)

_Branch: `feat/tcc-progress`. Written 2026-05-29. Compacted session context for pickup._

## Why this work happened

The v0.8 line landed in-OS TCC: `tcc.elf` runs on bare metal, calc.elf and
sh.elf rebuild themselves under it. The next obvious milestone — the
*kernel* rebuilds itself with the same compiler — was the question this
session set out to answer end-to-end.

## What shipped

Five commits on `feat/tcc-progress`:

1. **`feat(self-host): kernel rebuilds itself with TCC, in-OS driver script`**
   — the substantive change. Source tree is now dual-compatible (gcc + TCC);
   `./build-kernel-tcc.sh` builds a valid Multiboot 2 kernel ELF that QEMU
   boots end-to-end and runs all subsystem init cleanly.
2. **`fix(heap,fd): defensive guards survive in-OS rebuild's fork/exec stress`**
   — `kfree` range-checks `ptr` + freelist `next`; `fd_close` skips kfrees on
   bogus `data` ptrs. Diagnostic serial output names the caller so future
   bugs are easy to triage.
3. **`fix(heap): round kmalloc size up to 4-byte alignment`** — the root-cause
   fix. Without this, any `kmalloc(odd_size)` produced a misaligned remainder
   block on split; the misalignment cascaded forward, eventually poisoning the
   freelist and panicking the kernel under heavy fork/exec churn. One-line
   fix: `size = (size + 3) & ~3` at kmalloc entry.
4. **`feat(test): opt-in REBUILD-KERNEL phase in test_mode`** — wires the
   in-OS rebuild script into `test_mode` via a `TEST_WANT_EXPLICIT` opt-in,
   so the same `Serial_WriteString` marker discipline (`REBUILD-KERNEL: ALL
   PASS / FAIL`) used by INCORE/LIBC-TCC/SHELL-SMOKE applies.
5. **`fix(rebuild-kernel.sh): bareword markers, FAILED accumulator, static kend.S`**
   — three kernel-sh quirks the first generator missed: (a) `echo "X"` keeps
   the literal quotes, (b) `> file` redirection isn't implemented, (c) `exit
   N` from inside `if` doesn't abort. Fixed: bareword markers, ship `kend.S`
   as a static file, accumulate failures into a `FAILED` variable, final IF
   decides PASS / FAIL.

## Kernel source changes that crossed both compilers

All TCC-compatibility work stays gcc-clean (`./run.sh iso test` is fully
green: `KTEST_RESULT: PASS`, `INCORE: ALL PASS`, `LIBC-TCC: ALL PASS`,
`SHELL-SMOKE: ALL PASS`, `ui_test: 20/20`, `==> All ISO tests PASSED.`):

- **`vendor/tinycc/tccasm.c`** — NULL-guarded `asm_expr_sum` so forward
  symbol refs error cleanly instead of segfaulting TCC itself.
- **`boot.S`** — hardcoded `MB2_HEADER_LEN = 48` (TCC asm can't resolve
  forward refs); `#ifdef __TINYC__` routes the multiboot header into `.text`
  so the linker (no script support) keeps it in the first 32 KiB of the
  binary. New `mb2_header_check.c` statically asserts the layout matches.
- **`chainload.S`** — same hardcode trick for the far-jmp offset; gcc still
  validates the symbolic form.
- **`isr_asm.S`** — GAS `.macro` rewritten as cpp `#define` so TCC's
  assembler can handle it; `#ifndef __TINYC__` skips `.extern` (GAS no-op).
- **`kernel/atomic.h`** (new) — TCC-only shims for every `__atomic_*`
  builtin in vtty.c (`load_n`, `store_n`, `fetch_add`, `exchange_n`,
  `compare_exchange_n`, `add_fetch`) plus `__builtin_unreachable`, using
  single-CPU i386 `lock`-prefixed asm.
- **`syscall.c`** — hoisted `SYS_READDIR`'s nested-function callback to file
  scope (TCC rejects gcc nested functions).
- **`libc/stdlib/abort.c`** — drops `__builtin_unreachable()` under TCC.
- **`kernel.c`** — banner prints `Self-hosted kernel! (TCC build)` or
  `Host-built kernel (GCC build)` based on `__TINYC__`; serial marker
  `kernel: build=tcc` vs `kernel: build=gcc`.

## ISO staging

`iso.sh` and `build-tcc.sh` now stage:

- `/usr/include/kernel-build/kernel/**` — kernel headers re-exposed so the
  in-OS rebuild script can `-I/usr/include/kernel-build`.
- `/usr/lib/tcc/include/{stdint.h,limits.h}` — Makar-flavoured stubs that
  upstream TCC doesn't ship.
- `/apps/rebuild-kernel.sh` — the generated in-OS driver.
- `/apps/kend.S` — the `_kernel_end` sentinel source (kernel sh has no `>`
  redirection so we can't generate it at runtime).

## How to verify

### Host-side (~30s)

```sh
./build-kernel-tcc.sh
# → build/ktcc/makar.kernel.tcc  (356884 bytes, MB2-validated)
```

Boot it: `qemu-system-i386 -kernel build/ktcc/makar.kernel.tcc -serial stdio -display none` (or build an ISO from it; the spike used `grub-mkrescue`).

### In-OS (~5–15 min in TCG QEMU)

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  arawn780/gcc-cross-i686-elf:fast \
  bash -c "TEST_CMDLINE='test_mode test=rebuild-kernel' CFLAGS='-O0 -g3' TEST_ISO=1 bash iso.sh"

qemu-system-i386 -cdrom makar-test.iso -display none \
  -serial file:/tmp/rk.log -no-reboot -m 256
# Wait for "REBUILD-KERNEL: ALL PASS" or "REBUILD-KERNEL: FAIL" in /tmp/rk.log
```

## Open issues (handed off)

### Per-file compile failures in-OS — task #16

Most `.c` and `.S` files compile fine in-OS, but ~19/75 fail with
`(null):3811692: error: invalid number syntax`. The `(null)` source name +
huge line number suggest `tcc.elf` is reading a null/corrupt filename
pointer. Same `tcc.elf` works fine when invoked directly from an
interactive shell, so the bug is in the **kernel-sh script's `exec`
argv-passing path** (`sh_script.c` → `shell_exec_elf` → `exec_task_entry`),
likely a lifetime issue with argv buffers across the fork boundary or a
shared-static-globals re-entry race when many `exec`s come back to back from
the same script.

When this is fixed, the in-OS rebuild should produce `REBUILD-KERNEL: ALL
PASS` and a usable `/tmp/makar.kernel.tcc` that can be copied over
`/boot/makar.kernel` on the FAT32 boot partition for the next reboot.

### One stress-revealed heap warning — separate from #16

Under the full 75-invocation rebuild workload, a single
`kfree: corrupt next` event fires (block 0x82E02C, size field contains
ASCII bytes `/lc?`). The defensive guard catches it; the kernel survives.
Looks like an unrelated buffer-overrun in some path, lower priority than
#16.

## Files touched

```
build-kernel-tcc.sh                       (new)  host driver + in-OS script generator
build-tcc.sh                                     stage stdint.h/limits.h stubs
iso.sh                                           stage kernel-build/ headers
src/userspace/Makefile                           install rebuild-kernel.sh + kend.S
src/userspace/rebuild-kernel.sh           (new)  generated in-OS driver (do not edit)
src/userspace/kend.S                      (new)  _kernel_end sentinel
src/kernel/arch/i386/boot/boot.S                 __TINYC__ ifdef for .text header
src/kernel/arch/i386/boot/mb2_header_check.c (new) layout assertion
src/kernel/arch/i386/core/isr_asm.S              GAS .macro → cpp #define
src/kernel/arch/i386/drivers/keyboard.c          include atomic.h
src/kernel/arch/i386/make.config                 add mb2_header_check.o
src/kernel/arch/i386/mm/heap.c                   kmalloc 4-byte align + kfree guards
src/kernel/arch/i386/proc/chainload.S            __TINYC__ ifdef hardcoded offset
src/kernel/arch/i386/proc/fd.c                   fd_close guard
src/kernel/arch/i386/proc/syscall.c              hoist nested SYS_READDIR cb
src/kernel/arch/i386/proc/vtty.c                 include atomic.h
src/kernel/include/kernel/atomic.h        (new)  TCC-only __atomic_* shims
src/kernel/include/kernel/version.h              MAKAR_VERSION 0.8.1 → 0.9.0
src/kernel/kernel/kernel.c                       build banner + REBUILD-KERNEL phase
src/libc/stdlib/abort.c                          drop __builtin_unreachable under TCC
vendor/tinycc/build-stubs/limits.h        (new)  TCC missing-header stub
vendor/tinycc/tccasm.c                           NULL-guard forward-ref subtraction
docs/handoff-self-hosting.md              (new)  this file
```
