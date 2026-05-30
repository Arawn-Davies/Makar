#ifndef _KERNEL_ATOMIC_H
#define _KERNEL_ATOMIC_H

/* Under gcc, the kernel uses native __atomic_* builtins.  TCC has no
 * implementation, so when building with TCC we provide single-CPU i386
 * equivalents via `lock`-prefixed asm.  Memory-order arguments are
 * accepted but ignored (single-CPU + aligned word ops). */

#ifdef __TINYC__

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

#define __atomic_load_n(p, ord)      (*(volatile __typeof__(*(p)) *)(p))
#define __atomic_store_n(p, v, ord)  ((void)(*(volatile __typeof__(*(p)) *)(p) = (v)))

static inline int __atomic_kshim_fetch_add(volatile int *p, int v)
{
    int r;
    __asm__ volatile("lock xaddl %0, %1"
        : "=r"(r), "+m"(*p)
        : "0"(v)
        : "memory");
    return r;
}
#define __atomic_fetch_add(p, v, ord) \
    __atomic_kshim_fetch_add((volatile int *)(p), (v))

static inline int __atomic_kshim_exchange(volatile int *p, int v)
{
    int r;
    __asm__ volatile("xchgl %0, %1"
        : "=r"(r), "+m"(*p)
        : "0"(v)
        : "memory");
    return r;
}
#define __atomic_exchange_n(p, v, ord) \
    __atomic_kshim_exchange((volatile int *)(p), (v))

static inline int __atomic_kshim_cmpxchg(volatile int *p, int *expected, int desired)
{
    int prev, want = *expected;
    __asm__ volatile("lock cmpxchgl %2, %1"
        : "=a"(prev), "+m"(*p)
        : "r"(desired), "0"(want)
        : "memory");
    if (prev == want) return 1;
    *expected = prev;
    return 0;
}
#define __atomic_compare_exchange_n(p, exp, des, weak, succ, fail) \
    __atomic_kshim_cmpxchg((volatile int *)(p), (int *)(exp), (int)(des))

static inline int __atomic_kshim_add_fetch(volatile int *p, int v)
{
    int r;
    __asm__ volatile("lock xaddl %0, %1"
        : "=r"(r), "+m"(*p)
        : "0"(v)
        : "memory");
    return r + v;
}
#define __atomic_add_fetch(p, v, ord) \
    __atomic_kshim_add_fetch((volatile int *)(p), (v))

/* TCC 0.9.27 has no __builtin_unreachable.  Treat as a no-op: the surrounding
 * code typically already loops/halts, and unreachable() is only a hint. */
#define __builtin_unreachable() do { } while (0)

#endif /* __TINYC__ */

#endif
