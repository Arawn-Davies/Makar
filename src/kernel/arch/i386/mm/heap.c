#include <kernel/heap.h>
#include <kernel/paging.h>
#include <kernel/serial.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Block header for the first-fit linked-list allocator.
 *
 * Each allocation is preceded by a block_hdr_t in memory:
 *
 *   [ block_hdr_t | <user data> ] [ block_hdr_t | <user data> ] …
 *
 * `size`    – bytes of user data in this block (does not include the header).
 * `is_free` – non-zero when this block is available for allocation.
 * `next`    – pointer to the immediately following block header, or NULL for
 *             the last block in the list.
 * --------------------------------------------------------------------------- */
typedef struct block_hdr {
    size_t            size;
    uint32_t          is_free;
    struct block_hdr *next;
} block_hdr_t;

#define BLOCK_HDR_SIZE  sizeof(block_hdr_t)

/* Minimum user-data size kept when splitting a block.  Splitting a block that
   would leave a remainder smaller than this wastes less memory by not splitting
   at all. */
#define SPLIT_MIN  16

static block_hdr_t *heap_head = NULL;

/* ---------------------------------------------------------------------------
 * heap_init
 * --------------------------------------------------------------------------- */
void heap_init(void)
{
    /* Map the entire heap region before touching any byte of it. */
    paging_map_region(HEAP_START, HEAP_MAX - HEAP_START);

    /* Place one large free block covering the whole heap. */
    heap_head = (block_hdr_t *)HEAP_START;
    heap_head->size    = HEAP_MAX - HEAP_START - BLOCK_HDR_SIZE;
    heap_head->is_free = 1;
    heap_head->next    = NULL;

    KLOG("heap_init: ");
    KLOG_DEC((HEAP_MAX - HEAP_START) / (1024 * 1024));
    KLOG(" MiB @ ");
    KLOG_HEX(HEAP_START);
    KLOG("\n");
}

/* ---------------------------------------------------------------------------
 * kmalloc – first-fit allocator
 * --------------------------------------------------------------------------- */
void *kmalloc(size_t size)
{
    if (size == 0 || heap_head == NULL)
        return NULL;

    block_hdr_t *blk = heap_head;

    while (blk) {
        if (blk->is_free && blk->size >= size) {
            /* Split the block if the remainder is large enough to be useful. */
            if (blk->size >= size + BLOCK_HDR_SIZE + SPLIT_MIN) {
                block_hdr_t *remainder =
                    (block_hdr_t *)((uint8_t *)blk + BLOCK_HDR_SIZE + size);
                remainder->size    = blk->size - size - BLOCK_HDR_SIZE;
                remainder->is_free = 1;
                remainder->next    = blk->next;

                blk->size = size;
                blk->next = remainder;
            }

            blk->is_free = 0;
            return (void *)((uint8_t *)blk + BLOCK_HDR_SIZE);
        }
        blk = blk->next;
    }

    return NULL; /* heap exhausted */
}

/* ---------------------------------------------------------------------------
 * kfree – mark block free and coalesce with the immediately following block
 * --------------------------------------------------------------------------- */
void kfree(void *ptr)
{
    if (!ptr)
        return;

    /* Reject obviously-bogus pointers before dereferencing them.  Caught
     * a stress-induced bug under heavy rebuild-kernel.sh fork/exec load
     * where a non-heap pointer reached kfree and SIGSEGV'd the kernel.
     * Print the caller so the upstream owner can be found. */
    uintptr_t up = (uintptr_t)ptr;
    if (up < HEAP_START + BLOCK_HDR_SIZE || up >= HEAP_MAX) {
        Serial_WriteString("kfree: BAD POINTER ");
        Serial_WriteHex(up);
        Serial_WriteString(" caller=");
        Serial_WriteHex((uintptr_t)__builtin_return_address(0));
        Serial_WriteString("\n");
        return;
    }

    block_hdr_t *blk = (block_hdr_t *)((uint8_t *)ptr - BLOCK_HDR_SIZE);
    blk->is_free = 1;

    /* Coalesce with the next block if it is also free.  Validate next-pointer
     * lies within the heap before deref -- a wild write from elsewhere can
     * clobber a free block's `next` field, and following it would page-fault
     * in the kernel.  Report the corrupt block so the upstream owner can be
     * traced, then stop coalescing. */
    while (blk->next) {
        uintptr_t np = (uintptr_t)blk->next;
        if (np < HEAP_START || np >= HEAP_MAX) {
            Serial_WriteString("kfree: corrupt next in blk=");
            Serial_WriteHex((uintptr_t)blk);
            Serial_WriteString(" size=");
            Serial_WriteHex((uintptr_t)blk->size);
            Serial_WriteString(" next=");
            Serial_WriteHex(np);
            Serial_WriteString(" caller=");
            Serial_WriteHex((uintptr_t)__builtin_return_address(0));
            Serial_WriteString("\n");
            blk->next = NULL;   /* prevent further deref; sacrifice some heap */
            break;
        }
        if (!blk->next->is_free)
            break;
        blk->size += BLOCK_HDR_SIZE + blk->next->size;
        blk->next  = blk->next->next;
    }
}

/* ---------------------------------------------------------------------------
 * krealloc
 * --------------------------------------------------------------------------- */
void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_hdr_t *blk = (block_hdr_t *)((uint8_t *)ptr - BLOCK_HDR_SIZE);

    /* If the current block is already large enough, keep it. */
    if (blk->size >= size)
        return ptr;

    void *new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, blk->size < size ? blk->size : size);
    kfree(ptr);
    return new_ptr;
}

/* ---------------------------------------------------------------------------
 * Diagnostic helpers
 * --------------------------------------------------------------------------- */
size_t heap_used(void)
{
    size_t used = 0;
    for (block_hdr_t *b = heap_head; b; b = b->next)
        if (!b->is_free)
            used += b->size;
    return used;
}

size_t heap_free(void)
{
    size_t free_bytes = 0;
    for (block_hdr_t *b = heap_head; b; b = b->next)
        if (b->is_free)
            free_bytes += b->size;
    return free_bytes;
}

/* GDB calls vfs_file_exists("literal") which requires malloc/free to exist
 * in the target so GDB can allocate space for the string.  Map them to the
 * kernel allocator. */
void *malloc(size_t size) { return kmalloc(size); }
void  free(void *ptr)     { kfree(ptr); }
