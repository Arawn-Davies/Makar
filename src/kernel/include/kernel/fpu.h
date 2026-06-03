#ifndef _KERNEL_FPU_H
#define _KERNEL_FPU_H

/*
 * x87 FPU bring-up (prep for a hosted libc -- printf("%f"), strtod, <math.h>).
 *
 * fpu_init() arms the floating-point unit at boot: CR0.EM=0 (use the real
 * FPU, not emulation), CR0.MP=1, CR0.TS=0 (no lazy-switch trap yet), `fninit`
 * to a known state, and CR4.OSFXSR when FXSAVE/FXRSTOR is available.
 *
 * IMPORTANT: per-task x87 state is NOT yet saved/restored across context
 * switches.  This only enables the unit.  It is safe today because nothing
 * runs floating point concurrently -- the kernel is integer-only and the
 * in-tree userspace apps are too.  Before hosted (musl/newlib) binaries start
 * doing FP, the scheduler must fxsave/fxrstor a per-task state area (see
 * toolchain/README.md, "x87" gap).
 */
void fpu_init(void);

/*
 * Self-test helpers, written in inline x87 asm so they don't depend on the
 * kernel's C floating-point codegen.  Used by the `test_fpu` ktest suite to
 * prove the unit is live after fpu_init().
 */
int fpu_imul(int a, int b);   /* round(a * b)   via fild/fmulp/fistp */
int fpu_isqrt(int x);         /* round(sqrt(x)) via fild/fsqrt/fistp */

#endif /* _KERNEL_FPU_H */
