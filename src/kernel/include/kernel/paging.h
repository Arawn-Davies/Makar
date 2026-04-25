#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

/* Set up paging:
 *   - enable CR4.PSE (Page Size Extensions)
 *   - identity-map the first 256 MiB using 4 MiB large pages (PS bit set),
 *     mirroring the large-page strategy of 64-bit kernels (2 MiB in long mode)
 *   - register a page-fault handler (ISR 14) that panics with the faulting address
 *   - enable paging by loading CR3 and setting CR0.PG
 */
void paging_init(void);

/*
 * Identity-map an arbitrary physical address range [phys_start, phys_start+size)
 * using a static pool of extra 4 KiB page tables.  Safe to call after paging_init().
 * Ranges that fall within the initial 256 MiB large-page window are silently
 * skipped (already mapped).  Returns without mapping anything if the internal
 * page-table pool is exhausted.
 */
void paging_map_region(uint32_t phys_start, uint32_t size);

/* Returns a pointer to the kernel's page directory. Used by vmm_create_pd()
   to propagate kernel PDEs into new per-process page directories. */
uint32_t *paging_kernel_pd(void);

#endif /* _KERNEL_PAGING_H */
