#ifndef _KERNEL_VMM_H
#define _KERNEL_VMM_H

#include <stdint.h>

/* Flags for vmm_map_page (combine as needed). */
#define VMM_FLAG_WRITABLE  0x2u   /* page is writable             */
#define VMM_FLAG_USER      0x4u   /* accessible from ring 3       */

/*
 * vmm_create_pd – allocate a fresh 4 KiB-aligned page directory.
 *
 * Allocates one physical frame from the PMM, zeroes it, then copies the
 * kernel identity-map PDEs (0–256 MiB, indices 0–63) so the kernel remains
 * accessible from every process.  Returns the PD address (physical ==
 * virtual because the kernel is identity-mapped), or NULL on PMM exhaustion.
 */
uint32_t *vmm_create_pd(void);

/*
 * vmm_map_page – install one 4 KiB page mapping in a page directory.
 *
 * Maps virtual address `virt` to physical address `phys` with `flags`
 * (VMM_FLAG_* combined).  Allocates a page table from the PMM if the
 * relevant PDE slot is not yet present.  Silently ignores mappings that
 * fall within the kernel large-page window (PDE indices 0–63).
 */
void vmm_map_page(uint32_t *pd, uint32_t virt, uint32_t phys, uint32_t flags);

/*
 * vmm_unmap_page – remove one 4 KiB mapping from a page directory.
 *
 * Clears the PTE for `virt` and issues `invlpg` if `pd` is currently the
 * active page directory.  No-op if the PDE or PTE is absent.
 */
void vmm_unmap_page(uint32_t *pd, uint32_t virt);

/*
 * vmm_switch – activate a page directory by loading it into CR3.
 */
void vmm_switch(uint32_t *pd);

/*
 * vmm_free_pd – release all resources owned by a process page directory.
 *
 * Walks every non-kernel PDE, frees each mapped user page via the PMM,
 * frees each page-table frame, then frees the page-directory frame itself.
 * Must not be called while `pd` is the active CR3.
 */
void vmm_free_pd(uint32_t *pd);

#endif /* _KERNEL_VMM_H */
