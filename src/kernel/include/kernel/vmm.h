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
 * vmm_unmap_and_free – like vmm_unmap_page, but also releases the page's
 * backing frame via pmm_free_frame (refcount-decrement; freed at zero).  Use
 * for mappings the task owns a ref on: anonymous and file-backed mmap pages,
 * including shared page-cache frames.  No-op if the PDE/PTE is absent.
 */
void vmm_unmap_and_free(uint32_t *pd, uint32_t virt);

/*
 * vmm_protect_page – rewrite the permission bits of an existing mapping,
 * keeping its physical frame (backs mprotect()).  No-op if unmapped.
 */
void vmm_protect_page(uint32_t *pd, uint32_t virt, uint32_t flags);

/*
 * vmm_switch – activate a page directory by loading it into CR3.
 */
void vmm_switch(uint32_t *pd);

/* Software-defined PTE bit, signals a page is currently in COW state.
 *
 * The CPU ignores bits 9-11 of a PTE (the "available" bits in the Intel
 * SDM), so we use bit 9 to remember that a page was made read-only purely
 * for copy-on-write purposes -- the #PF handler distinguishes a real
 * read-only-protection fault from a COW fault by checking this bit. */
#define VMM_PTE_COW   0x200u

/*
 * vmm_clone_pd_cow – clone a page directory using copy-on-write.
 *
 * Allocates a fresh PD, mirrors kernel PDEs (shared, identity-mapped),
 * and for each user-installed page table:
 *   - allocates a fresh PT frame in the child
 *   - for each present PTE in the parent's PT:
 *       - bumps the frame's refcount via pmm_inc_ref
 *       - clears PAGE_WRITABLE and sets VMM_PTE_COW in BOTH parent and
 *         child PTEs (so a write from either task takes a #PF)
 *       - mirrors the resulting PTE into the child's PT
 *
 * If the parent PD is currently loaded in CR3, the TLB is flushed so the
 * new read-only bits take effect immediately.
 *
 * Returns the child PD (physical == virtual), or NULL on PMM exhaustion;
 * on failure all partially-allocated frames are released.
 */
uint32_t *vmm_clone_pd_cow(uint32_t *parent_pd);

/*
 * vmm_free_pd – release all resources owned by a process page directory.
 *
 * Walks every non-kernel PDE, frees each mapped user page via the PMM,
 * frees each page-table frame, then frees the page-directory frame itself.
 * Must not be called while `pd` is the active CR3.
 */
void vmm_free_pd(uint32_t *pd);

/*
 * vmm_count_user_pages – count resident user pages mapped in `pd`.
 *
 * Walks every 4 KiB (non-large) present PDE and counts PTEs that are both
 * PRESENT and USER, i.e. the task's resident user memory (an RSS-style
 * figure; COW-shared frames are counted in each sharing task).  Returns 0
 * for a NULL pd or a kernel-only task.  Used by /proc/tasks and maktop.
 */
uint32_t vmm_count_user_pages(uint32_t *pd);

#endif /* _KERNEL_VMM_H */
