#ifndef _KERNEL_HEAP_H
#define _KERNEL_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* The heap occupies the virtual address range immediately above the
   8 MiB identity-mapped boot window.  heap_init() maps this region via
   paging_map_region() before setting up the free-list. */
#define HEAP_START  0x800000u   /*  8 MiB – first byte past the boot mapping */
#define HEAP_MAX    0x3000000u  /* 48 MiB – exclusive upper bound (40 MiB heap).
                                 * Sized for a full 12 MiB Doom IWAD loaded whole
                                 * into a kmalloc buffer.  QEMU now runs -m 64, so
                                 * the whole heap is backed by real RAM (at -m 32
                                 * the top of the heap was unbacked, truncating
                                 * large file loads).  ~16 MiB left for ring-3. */

/*
 * Initialise the heap.  Must be called after paging_init() and before any
 * call to kmalloc / kfree.  Maps the entire heap region into the virtual
 * address space and installs one large free block covering it all.
 */
void heap_init(void);

/* Allocate at least `size` bytes.  Returns NULL if the heap is exhausted. */
void *kmalloc(size_t size);

/* Release a block previously returned by kmalloc or krealloc.
   Coalesces adjacent free blocks to reduce fragmentation.
   Passing NULL is a no-op. */
void kfree(void *ptr);

/*
 * Resize an existing allocation.  If ptr is NULL, behaves like kmalloc(size).
 * If size is 0, behaves like kfree(ptr) and returns NULL.
 * Returns NULL and leaves ptr untouched on allocation failure.
 */
void *krealloc(void *ptr, size_t size);

/* Diagnostic helpers for a future meminfo command. */
size_t heap_used(void);
size_t heap_free(void);

#endif /* _KERNEL_HEAP_H */
