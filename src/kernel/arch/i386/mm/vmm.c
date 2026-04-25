#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <string.h>

/*
 * Number of kernel PDEs to propagate into each new page directory.
 * 64 × 4 MiB PSE entries = 256 MiB, matching the identity window in paging.c.
 */
#define KERNEL_PDE_COUNT  64u

/* Mirrors the page-entry flag bits used by paging.c. */
#define PAGE_PRESENT   0x1u
#define PAGE_WRITABLE  0x2u
#define PAGE_USER      0x4u
#define PAGE_LARGE     0x80u

uint32_t *vmm_create_pd(void)
{
    uint32_t phys = pmm_alloc_frame();
    if (phys == PMM_ALLOC_ERROR)
        return NULL;

    /* Physical == virtual: the kernel is identity-mapped. */
    uint32_t *pd = (uint32_t *)phys;
    memset(pd, 0, PMM_FRAME_SIZE);

    /* Share kernel PDEs so the kernel is visible from this address space. */
    uint32_t *kpd = paging_kernel_pd();
    for (uint32_t i = 0; i < KERNEL_PDE_COUNT; i++)
        pd[i] = kpd[i];

    return pd;
}

void vmm_map_page(uint32_t *pd, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FFu;

    /* Refuse to overwrite the kernel's large-page entries. */
    if (pd[pdi] & PAGE_LARGE)
        return;

    if (!(pd[pdi] & PAGE_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (pt_phys == PMM_ALLOC_ERROR)
            return;
        memset((void *)pt_phys, 0, PMM_FRAME_SIZE);
        /* PDE is writable + user so ring-3 can walk into it. */
        pd[pdi] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    pt[pti] = (phys & ~0xFFFu) | (flags & 0xFFFu) | PAGE_PRESENT;
}

void vmm_unmap_page(uint32_t *pd, uint32_t virt)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FFu;

    if (!(pd[pdi] & PAGE_PRESENT) || (pd[pdi] & PAGE_LARGE))
        return;

    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    if (!(pt[pti] & PAGE_PRESENT))
        return;

    pt[pti] = 0;

    /* Flush TLB entry only if this PD is currently loaded. */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == (uint32_t)pd)
        asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_switch(uint32_t *pd)
{
    asm volatile("mov %0, %%cr3" :: "r"((uint32_t)pd) : "memory");
}

void vmm_free_pd(uint32_t *pd)
{
    /* Only walk non-kernel PDE slots (skip indices 0–63). */
    for (uint32_t pdi = KERNEL_PDE_COUNT; pdi < 1024; pdi++) {
        if (!(pd[pdi] & PAGE_PRESENT) || (pd[pdi] & PAGE_LARGE))
            continue;

        uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);

        for (uint32_t pti = 0; pti < 1024; pti++) {
            if (pt[pti] & PAGE_PRESENT)
                pmm_free_frame(pt[pti] & ~0xFFFu);
        }

        pmm_free_frame((uint32_t)pt);
    }

    pmm_free_frame((uint32_t)pd);
}
