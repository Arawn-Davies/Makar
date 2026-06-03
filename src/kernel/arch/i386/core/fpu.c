/*
 * fpu.c -- x87 FPU bring-up.  See <kernel/fpu.h>.
 */
#include <kernel/fpu.h>
#include <stdint.h>

static inline uint32_t cpuid1_edx(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u));
    (void)a; (void)b; (void)c;
    return d;
}

void fpu_init(void)
{
    /* CR0: EM=0 (real FPU), MP=1, TS=0 (no lazy-FPU trap yet). */
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);   /* EM */
    cr0 |=  (1u << 1);   /* MP */
    cr0 &= ~(1u << 3);   /* TS */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    /* Bring the x87 stack/status to a known state. */
    __asm__ volatile("fninit");

    /* Enable FXSAVE/FXRSTOR (and SSE FP exceptions) when supported -- needed
     * for the per-task save/restore that comes with hosted FP. */
    uint32_t edx = cpuid1_edx();
    if (edx & (1u << 24)) {          /* FXSR */
        uint32_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 9);            /* OSFXSR */
        if (edx & (1u << 25))        /* SSE */
            cr4 |= (1u << 10);       /* OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }
}

int fpu_imul(int a, int b)
{
    int out;
    __asm__ volatile(
        "fildl %1\n\t"
        "fildl %2\n\t"
        "fmulp %%st, %%st(1)\n\t"
        "fistpl %0\n\t"
        : "=m"(out)
        : "m"(a), "m"(b)
        : "memory", "st", "st(1)");
    return out;
}

int fpu_isqrt(int x)
{
    int out;
    __asm__ volatile(
        "fildl %1\n\t"
        "fsqrt\n\t"
        "fistpl %0\n\t"
        : "=m"(out)
        : "m"(x)
        : "memory", "st");
    return out;
}
