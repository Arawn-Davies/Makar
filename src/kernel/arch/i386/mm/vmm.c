#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <string.h>

/* Mirrors the page-entry flag bits used by paging.c. */
#define PAGE_PRESENT   0x1u
#define PAGE_WRITABLE  0x2u
#define PAGE_USER      0x4u
#define PAGE_LARGE     0x80u

/* Higher-half kernel virtual base (must match KERNEL_VBASE in linker.ld /
 * boot.S / paging.c).  Per-task user page directories come from
 * pmm_alloc_frame() and so are addressed by their (low) physical address,
 * which equals their virtual address via the permanent identity map.  The
 * kernel's own page directory, however, is a kernel static linked high; its
 * pointer is >= KERNEL_VBASE and must be converted to a physical address
 * before it can be loaded into CR3.  v2p_pd() does that translation. */
#ifdef __TINYC__
#define KERNEL_VBASE   0x00000000u   /* low-half TCC build: identity */
#else
#define KERNEL_VBASE   0xC0000000u
#endif

static inline uint32_t v2p_pd(uint32_t *pd)
{
    uint32_t a = (uint32_t)pd;
    return (a >= KERNEL_VBASE) ? (a - KERNEL_VBASE) : a;
}

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

/* vmm_protect_page – change the permission bits of an existing mapping while
 * keeping its physical frame.  No-op if the page isn't mapped.  Backs
 * mprotect() (musl's dynamic linker re-protects RELRO after relocation).
 * Flushes the TLB for `virt` (the calling task owns the active address space). */
void vmm_protect_page(uint32_t *pd, uint32_t virt, uint32_t flags)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FFu;

    if (pd[pdi] & PAGE_LARGE)      return;
    if (!(pd[pdi] & PAGE_PRESENT)) return;
    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    if (!(pt[pti] & PAGE_PRESENT)) return;

    uint32_t phys = pt[pti] & ~0xFFFu;
    pt[pti] = phys | (flags & 0xFFFu) | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
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

    /* Flush TLB entry only if this PD is currently loaded.  CR3 holds a
       physical address; translate pd before comparing. */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == v2p_pd(pd))
        asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_switch(uint32_t *pd)
{
    /* CR3 needs a physical address.  User PDs are physical already; the kernel
       PD is a high-linked static and must be translated down. */
    asm volatile("mov %0, %%cr3" :: "r"(v2p_pd(pd)) : "memory");
}

/* Kernel identity-map upper bound (paging.c maps 0-256 MiB via 4 MiB PSE
 * large pages).  A PDE pointing outside this range cannot be safely walked
 * by the kernel -- the PT-pointer dereference would itself page-fault.
 * Any PDE whose physical address is >= this bound is treated as corrupt
 * and skipped.  Real user-task PTs always come from pmm_alloc_frame which
 * only hands out frames within this identity-mapped window. */
#define VMM_KERNEL_IDMAP_END 0x10000000u

uint32_t *vmm_clone_pd_cow(uint32_t *parent_pd)
{
    uint32_t *kpd = paging_kernel_pd();

    /* Child PD: zero, then mirror kernel PDEs.  User PDEs filled below. */
    uint32_t child_phys = pmm_alloc_frame();
    if (child_phys == PMM_ALLOC_ERROR)
        return NULL;
    uint32_t *child = (uint32_t *)child_phys;
    memset(child, 0, PMM_FRAME_SIZE);
    for (uint32_t i = 0; i < 1024; i++) {
        if (kpd[i])
            child[i] = kpd[i];
    }

    /* Walk parent's user PDEs and clone any that diverge from kpd. */
    for (uint32_t pdi = 0; pdi < 1024; pdi++) {
        uint32_t ppde = parent_pd[pdi];

        if (!(ppde & PAGE_PRESENT) || (ppde & PAGE_LARGE))
            continue;
        /* PDE shared with kernel - already mirrored above, nothing to clone. */
        if (ppde == kpd[pdi])
            continue;

        uint32_t parent_pt_phys = ppde & ~0xFFFu;
        if (parent_pt_phys == 0 || parent_pt_phys >= VMM_KERNEL_IDMAP_END)
            continue;  /* corrupt parent PDE - skip, same defence as vmm_free_pd */

        uint32_t *parent_pt = (uint32_t *)parent_pt_phys;

        /* Allocate child PT.  On failure, unwind everything allocated so far. */
        uint32_t child_pt_phys = pmm_alloc_frame();
        if (child_pt_phys == PMM_ALLOC_ERROR) {
            vmm_free_pd(child);
            return NULL;
        }
        uint32_t *child_pt = (uint32_t *)child_pt_phys;
        memset(child_pt, 0, PMM_FRAME_SIZE);

        for (uint32_t pti = 0; pti < 1024; pti++) {
            uint32_t pte = parent_pt[pti];
            if (!(pte & PAGE_PRESENT))
                continue;

            uint32_t frame = pte & ~0xFFFu;

            if (pte & PAGE_USER) {
                /* COW: parent + child share one frame, both read-only. */
                pmm_inc_ref(frame);
                uint32_t cow_pte = (pte & ~PAGE_WRITABLE) | VMM_PTE_COW;
                parent_pt[pti] = cow_pte;
                child_pt[pti]  = cow_pte;
            } else {
                /* Kernel-only mapping sitting inside a user PT - just mirror.
                 * No refcount bump: the kernel never frees these via PMM
                 * during normal task teardown.  (vmm_free_pd does call
                 * pmm_free_frame on every present PTE, but kernel frames
                 * never go through pmm_alloc so their refcount stays 0 and
                 * pmm_free_frame becomes a no-op with a serial warning.
                 * In practice this branch should be unreachable.) */
                child_pt[pti] = pte;
            }
        }

        /* PDE flags from the parent, pointing at child's new PT. */
        child[pdi] = child_pt_phys | (ppde & 0xFFFu);
    }

    /* If parent is currently active, flush the TLB so the freshly-RO
     * PTEs take effect (otherwise the next parent write would silently
     * succeed against a cached writable TLB entry). */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == (uint32_t)parent_pd)
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

    return child;
}

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

uint32_t vmm_count_user_pages(uint32_t *pd)
{
    if (!pd)
        return 0;

    uint32_t pages = 0;
    for (uint32_t pdi = 0; pdi < 1024; pdi++) {
        uint32_t pde = pd[pdi];
        /* Kernel mappings use 4 MiB large pages (PAGE_LARGE) and are never
         * PAGE_USER, so skipping them excludes the kernel window.  Only
         * user page tables (allocated by vmm_map_page) are 4 KiB. */
        if (!(pde & PAGE_PRESENT) || (pde & PAGE_LARGE))
            continue;
        uint32_t pt_phys = pde & ~0xFFFu;
        if (pt_phys == 0 || pt_phys >= VMM_KERNEL_IDMAP_END)
            continue;

        uint32_t *pt = (uint32_t *)pt_phys;
        for (uint32_t pti = 0; pti < 1024; pti++) {
            uint32_t pte = pt[pti];
            if ((pte & PAGE_PRESENT) && (pte & PAGE_USER))
                pages++;
        }
    }
    return pages;
}
