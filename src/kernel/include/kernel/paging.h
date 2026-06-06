#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>

/* Higher-half kernel virtual base (must match KERNEL_VBASE in linker.ld,
 * boot.S, paging.c and vmm.c).  The in-OS TCC rebuild links the kernel
 * low-half (identity), so KERNEL_VBASE is 0 there. */
#ifdef __TINYC__
#define KERNEL_VBASE   0x00000000u
#else
#define KERNEL_VBASE   0xC0000000u
#endif

/* Physical address of a kernel pointer.  Kernel statics are linked high
 * (>= KERNEL_VBASE) and mapped 0xC0000000->0x0, so their physical address is
 * (virt - KERNEL_VBASE); anything already below KERNEL_VBASE is in the
 * permanent low identity window (phys == virt).  Use this for any value handed
 * to hardware that does physical addressing -- e.g. DMA PRD / PRDT entries. */
static inline uintptr_t kvirt_to_phys(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    return (a >= KERNEL_VBASE) ? (a - KERNEL_VBASE) : a;
}

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

/*
 * Same as paging_map_region(), but maps the range write-combining (WC) when the
 * CPU supports PAT (armed by paging_init()).  Used for the linear framebuffer so
 * pixel writes are batched instead of trapping as uncacheable MMIO on VT-x
 * hypervisors (VirtualBox, Hyper-V) and real hardware.  Falls back to a plain
 * mapping when PAT is unavailable.
 */
void paging_map_region_wc(uint32_t phys_start, uint32_t size);

/* Returns a pointer to the kernel's page directory. Used by vmm_create_pd()
   to propagate kernel PDEs into new per-process page directories. */
uint32_t *paging_kernel_pd(void);

#endif /* _KERNEL_PAGING_H */
