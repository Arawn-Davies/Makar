#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <string.h>

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

    /* Copy all present kernel PDEs so every kernel mapping (identity window,
     * VESA framebuffer, heap extra pages, etc.) is visible from this PD. */
    uint32_t *kpd = paging_kernel_pd();
    for (uint32_t i = 0; i < 1024; i++) {
        if (kpd[i])
            pd[i] = kpd[i];
    }

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

/* Kernel identity-map upper bound (paging.c maps 0-256 MiB via 4 MiB PSE
 * large pages).  A PDE pointing outside this range cannot be safely walked
 * by the kernel -- the PT-pointer dereference would itself page-fault.
 * Any PDE whose physical address is >= this bound is treated as corrupt
 * and skipped.  Real user-task PTs always come from pmm_alloc_frame which
 * only hands out frames within this identity-mapped window. */
#define VMM_KERNEL_IDMAP_END 0x10000000u

void vmm_free_pd(uint32_t *pd)
{
    uint32_t *kpd = paging_kernel_pd();
    for (uint32_t pdi = 0; pdi < 1024; pdi++) {
        uint32_t pde = pd[pdi];
        if (!(pde & PAGE_PRESENT) || (pde & PAGE_LARGE))
            continue;
        /* Skip PDEs shared with the kernel - freeing them would corrupt the
         * kernel's own mappings. */
        if (pde == kpd[pdi])
            continue;

        uint32_t pt_phys = pde & ~0xFFFu;

        /* Defence against corrupted PDEs: if the PT pointer lies outside
         * the kernel identity-map, walking it would fault.  Skip it and
         * log; if this ever fires it's a sign that something stomped on
         * the PD (use-after-free, double-mapping, etc.) and the upstream
         * cause needs investigating. */
        if (pt_phys == 0 || pt_phys >= VMM_KERNEL_IDMAP_END)
            continue;

        uint32_t *pt = (uint32_t *)pt_phys;

        for (uint32_t pti = 0; pti < 1024; pti++) {
            uint32_t pte = pt[pti];
            if (!(pte & PAGE_PRESENT))
                continue;
            uint32_t frame = pte & ~0xFFFu;
            if (frame && frame < VMM_KERNEL_IDMAP_END)
                pmm_free_frame(frame);
        }

        pmm_free_frame(pt_phys);
    }

    pmm_free_frame((uint32_t)pd);
}
