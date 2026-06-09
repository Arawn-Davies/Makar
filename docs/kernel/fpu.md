---
title: fpu
parent: Kernel subsystems
grand_parent: Reference
---

# fpu - x87/SSE FPU bring-up + per-task state

**Header:** `kernel/include/kernel/fpu.h`
**Source:** `arch/i386/core/fpu.c`

`fpu_init()` arms the floating-point unit at boot (prep for a hosted libc —
`printf("%f")`, `strtod`, `<math.h>`):

- `CR0.EM = 0` — use the real FPU, not emulation.
- `CR0.MP = 1`, `CR0.TS = 0` — no lazy-switch trap.
- `fninit` — known clean state.
- `CR4.OSFXSR` when `FXSAVE`/`FXRSTOR` is available.

The clean default state captured at init seeds each new task's save area.

---

## Per-task state

Each task carries a **512-byte, 16-byte-aligned** `FXSAVE` area (`FPU_STATE_SIZE
= 512`), saved/restored across context switches so concurrent FP-using tasks
don't clobber each other.

| Function | Role |
|---|---|
| `fpu_init()` | Arm the unit at boot. |
| `fpu_save(area_512)` | `fxsave` the current FPU/SSE state. |
| `fpu_restore(area_512)` | `fxrstor` a saved state. |
| `fpu_init_state(area_512)` | Seed a fresh task's area with the clean default. |
| `fpu_imul(a,b)` / `fpu_isqrt(x)` | Inline-x87 self-test helpers (`fild/fmulp/fistp`, `fild/fsqrt/fistp`) used by the `test_fpu` ktest to prove the unit is live. |

---

The scheduler calls `fpu_save`/`fpu_restore` on every context switch (see
[Internals §FPU/TLS](../internals.md)). The `test_fpu` ktest validates the unit
after `fpu_init()`. The `toolchain/README.md` "x87" note tracks the path toward
hosted FP binaries.
