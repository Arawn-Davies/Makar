#ifndef _KERNEL_PMM_H_
#define _KERNEL_PMM_H_

#include <stdint.h>
#include <kernel/multiboot.h>

#define PMM_FRAME_SIZE   0x1000        /* 4 KiB per frame */
#define PMM_ALLOC_ERROR  0xFFFFFFFF    /* returned when no frame is free */

/* Initialise the PMM from the Multiboot 2 memory map.
   Marks every frame as used, then frees usable regions, then re-marks
   the null page and all kernel frames as used. */
void     pmm_init(uint32_t magic, multiboot2_info_t *mbi);

/* Number of buddy orders.  Orders 0..PMM_MAX_ORDER-1; the largest block is
   2^(PMM_MAX_ORDER-1) frames = 4 MiB (matches Linux's default MAX_ORDER). */
#define PMM_MAX_ORDER 11

/* Allocate one physical frame.  Returns the physical address of the
   frame (always a multiple of PMM_FRAME_SIZE), or PMM_ALLOC_ERROR if
   no free frame is available.  Equivalent to pmm_alloc_pages(0). */
uint32_t pmm_alloc_frame(void);

/* Allocate 2^order physically-contiguous frames via the buddy allocator.
   The returned physical address is naturally aligned to the block size
   (2^order * PMM_FRAME_SIZE), which is what DMA ring buffers and page-table
   pools need.  Returns PMM_ALLOC_ERROR if no block of that order is free.
   order must be < PMM_MAX_ORDER. */
uint32_t pmm_alloc_pages(unsigned order);

/* Free a block previously returned by pmm_alloc_pages(order).  Drops one
   reference on the head frame and, when it reaches zero, returns the block
   to the buddy pool (coalescing with a free buddy where possible).  The
   order must match the original allocation. */
void     pmm_free_pages(uint32_t addr, unsigned order);

/* Diagnostic: the highest order whose free list is non-empty, or
   PMM_MAX_ORDER if the pool is entirely empty.  Lets ktests observe
   coalescing. */
unsigned pmm_largest_free_order(void);

/* Drop one reference to the physical frame at addr.
   When the refcount hits zero the frame is returned to the free pool.
   addr must be a value previously returned by pmm_alloc_frame(); calling
   this on a refcount-0 frame is a no-op (warned to serial in DEV_BUILD). */
void     pmm_free_frame(uint32_t addr);

/* Bump the refcount on a frame already owned by the caller.  Used by the
 * COW clone path so parent and child share a single physical frame until
 * one of them writes to it.  Must not be called on a refcount-0 frame. */
void     pmm_inc_ref(uint32_t addr);

/* Read the current refcount of a frame (0 = free).  Diagnostic / ktest. */
uint8_t  pmm_ref_count(uint32_t addr);

/* Return the number of frames currently in the free pool. */
uint32_t pmm_free_count(void);

/* Total managed frames (bootloader-available, minus null page + kernel
 * image).  Constant after pmm_init; used by /proc/meminfo's MemTotal. */
uint32_t pmm_managed_count(void);

#endif
