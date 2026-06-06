#include <kernel/pmm.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Physical memory manager -- Linux-style buddy allocator over a per-frame
 * page descriptor table (a lean `struct page`).
 *
 * The pool is a set of per-order free lists (orders 0..PMM_MAX_ORDER-1).
 * alloc_pages(order) returns 2^order physically-contiguous, naturally-aligned
 * frames by splitting a larger free block; free_pages coalesces a block with
 * its buddy whenever the buddy is also free at the same order, which keeps
 * large contiguous regions available (anti-fragmentation).
 *
 * The legacy single-frame API (pmm_alloc_frame / pmm_free_frame / pmm_inc_ref
 * / pmm_ref_count) is preserved exactly: a single frame is just an order-0
 * block, and per-frame reference counts (used by fork/COW) live in the page
 * descriptor's `refcount` field.
 *
 * Managed window: PMM_MAX_FRAMES caps the pool at 256 MiB, matching the
 * kernel's identity-mapped low region (paging_init maps 256 MiB with 4 MiB
 * pages).  Frames above that ceiling cannot be touched by the kernel without
 * a temporary mapping, so we don't manage them yet; lifting this is the job
 * of a future highmem/zone split.  Sizing the descriptor table to this window
 * (rather than the full 4 GiB the old bitmap covered) also shrinks the static
 * footprint to ~0.75 MiB.
 * ------------------------------------------------------------------------- */

#define PMM_MAX_FRAMES   (0x10000U)        /* 65 536 frames = 256 MiB window  */
#define PMM_NIL          0xFFFFFFFFU       /* free-list / index sentinel       */

#define PG_BUDDY  0x01                     /* frame is the free head of a block */

/* Per-frame descriptor.  Indexed by frame number = phys_addr / FRAME_SIZE.
 *
 *   next/prev : doubly-linked free-list links (frame indices), valid only
 *               while the frame is a free block head (PG_BUDDY set).
 *   order     : buddy order of the block this frame heads.  Set at alloc and
 *               while free; read back at free so callers cannot mismatch.
 *   refcount  : 0 = free.  Set to 1 on alloc; bumped by pmm_inc_ref for COW
 *               sharing; the block returns to the pool when it hits 0.
 *   flags     : PG_BUDDY etc.
 *
 * 12 bytes/frame * 65 536 frames = 768 KiB of BSS. */
typedef struct {
	uint32_t next;
	uint32_t prev;
	uint8_t  order;
	uint8_t  refcount;
	uint8_t  flags;
	uint8_t  _pad;
} page_t;

static page_t   mem_map[PMM_MAX_FRAMES];
static uint32_t free_lists[PMM_MAX_ORDER];  /* head frame index per order */
static uint32_t s_max_frames;               /* highest managed frame + 1   */
static uint32_t s_free_frames;              /* O(1) free-frame accounting   */
static uint32_t s_total_managed_frames;     /* constant after pmm_init      */

/* --------------------------------------------------------------------------
 * Free-list primitives (doubly-linked via frame indices)
 * -------------------------------------------------------------------------- */

static void list_add(uint32_t order, uint32_t f)
{
	uint32_t head = free_lists[order];
	mem_map[f].next = head;
	mem_map[f].prev = PMM_NIL;
	if (head != PMM_NIL)
		mem_map[head].prev = f;
	free_lists[order] = f;
	mem_map[f].order  = (uint8_t)order;
	mem_map[f].flags |= PG_BUDDY;
}

static void list_del(uint32_t order, uint32_t f)
{
	uint32_t p = mem_map[f].prev;
	uint32_t n = mem_map[f].next;
	if (p != PMM_NIL) mem_map[p].next = n; else free_lists[order] = n;
	if (n != PMM_NIL) mem_map[n].prev = p;
	mem_map[f].flags &= (uint8_t)~PG_BUDDY;
}

/* A frame is a coalescible buddy at `order` iff it is in range and currently
 * a free head of exactly that order. */
static int buddy_is_free(uint32_t b, uint32_t order)
{
	return b < s_max_frames
	    && (mem_map[b].flags & PG_BUDDY)
	    && mem_map[b].order == (uint8_t)order;
}

/* Return a block of 2^order frames (head frame index `f`) to the pool,
 * coalescing upward with free buddies. */
static void buddy_free(uint32_t f, uint32_t order)
{
	s_free_frames += (1U << order);

	while (order < PMM_MAX_ORDER - 1) {
		uint32_t buddy = f ^ (1U << order);
		if (!buddy_is_free(buddy, order))
			break;
		list_del(order, buddy);
		if (buddy < f)
			f = buddy;          /* merged block starts at the lower frame */
		order++;
	}

	mem_map[f].refcount = 0;
	list_add(order, f);
}

/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

void pmm_init(uint32_t magic, multiboot2_info_t *mbi)
{
	/* The kernel is linked higher-half; _kernel_phys_end is the PHYSICAL end
	 * of the loaded image (linker.ld computes it as _kernel_end - KERNEL_VBASE).
	 * Use it directly so the kernel reservation lands in low physical frames. */
	extern uint32_t _kernel_phys_end;

	/* Everything starts reserved: a zeroed descriptor table means refcount 0,
	 * no PG_BUDDY, off every free list -- i.e. not allocatable until freed. */
	memset(mem_map, 0, sizeof(mem_map));
	for (uint32_t o = 0; o < PMM_MAX_ORDER; o++)
		free_lists[o] = PMM_NIL;
	s_max_frames  = 0;
	s_free_frames = 0;

	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
		t_writestring("PMM: invalid multiboot2 magic, no memory freed\n");
		KLOG("pmm_init: invalid multiboot2 magic\n");
		return;
	}

	/* Locate the memory-map tag. */
	multiboot2_tag_mmap_t *mmap_tag = NULL;
	uint8_t *tag_ptr  = (uint8_t *)mbi + sizeof(multiboot2_info_t);
	uint8_t *info_end = (uint8_t *)mbi + mbi->total_size;

	while (tag_ptr < info_end) {
		multiboot2_tag_t *tag = (multiboot2_tag_t *)tag_ptr;
		if (tag->type == MULTIBOOT2_TAG_TYPE_END)
			break;
		if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
			mmap_tag = (multiboot2_tag_mmap_t *)tag;
			break;
		}
		tag_ptr += (tag->size + 7) & ~7u;
	}

	if (!mmap_tag) {
		t_writestring("PMM: no memory map tag from bootloader, no memory freed\n");
		KLOG("pmm_init: no memory map tag\n");
		return;
	}

	/* Reserved frames the bootloader-available map must not hand out: the
	 * null page (guards NULL derefs) and the whole kernel image, which on
	 * this build includes mem_map[] itself (static BSS below _kernel_end). */
	uint32_t kstart = 0x100000 / PMM_FRAME_SIZE;
	uint32_t kend   = ((uint32_t)&_kernel_phys_end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;

	/* Free every usable frame within the managed window into the buddy pool.
	 * Ascending order lets even/odd pairs coalesce as we go. */
	uint8_t *entry_ptr = (uint8_t *)mmap_tag + sizeof(multiboot2_tag_mmap_t);
	uint8_t *mmap_end  = (uint8_t *)mmap_tag + mmap_tag->size;

	while (entry_ptr < mmap_end) {
		multiboot2_mmap_entry_t *entry = (multiboot2_mmap_entry_t *)entry_ptr;

		if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
			uint64_t region_start =
				(entry->base_addr + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
			uint64_t region_end =
				(entry->base_addr + entry->length) & ~(uint64_t)(PMM_FRAME_SIZE - 1);

			for (uint64_t addr = region_start; addr < region_end; addr += PMM_FRAME_SIZE) {
				uint32_t f = (uint32_t)(addr / PMM_FRAME_SIZE);
				if (f >= PMM_MAX_FRAMES)
					break;                 /* past the 256 MiB window */
				if (f == 0 || (f >= kstart && f < kend))
					continue;              /* null page / kernel image */
				if (f + 1 > s_max_frames)
					s_max_frames = f + 1;
				buddy_free(f, 0);
			}
		}

		entry_ptr += mmap_tag->entry_size;
	}

	s_total_managed_frames = s_free_frames;

	t_writestring("PMM: ");
	t_dec(s_free_frames);
	t_writestring(" frames free (");
	t_dec((s_free_frames * PMM_FRAME_SIZE) / (1024 * 1024));
	t_writestring(" MiB), buddy max order ");
	t_dec(pmm_largest_free_order());
	t_putchar('\n');

	KLOG("pmm_init: ");
	KLOG_DEC(s_free_frames);
	KLOG(" frames free (");
	KLOG_DEC((s_free_frames * PMM_FRAME_SIZE) / (1024 * 1024));
	KLOG(" MiB)\n");
}

/* --------------------------------------------------------------------------
 * Allocation
 * -------------------------------------------------------------------------- */

uint32_t pmm_alloc_pages(unsigned order)
{
	if (order >= PMM_MAX_ORDER)
		return PMM_ALLOC_ERROR;

	/* Find the smallest available order >= the request. */
	unsigned o = order;
	while (o < PMM_MAX_ORDER && free_lists[o] == PMM_NIL)
		o++;
	if (o >= PMM_MAX_ORDER)
		return PMM_ALLOC_ERROR;           /* out of memory at this size */

	uint32_t f = free_lists[o];
	list_del(o, f);

	/* Split down to the requested order, parking each upper buddy half on the
	 * next lower free list. */
	while (o > order) {
		o--;
		uint32_t buddy = f + (1U << o);
		list_add(o, buddy);
	}

	mem_map[f].order    = (uint8_t)order;
	mem_map[f].refcount = 1;
	mem_map[f].flags   &= (uint8_t)~PG_BUDDY;
	s_free_frames      -= (1U << order);

	return f * PMM_FRAME_SIZE;
}

uint32_t pmm_alloc_frame(void)
{
	return pmm_alloc_pages(0);
}

/* --------------------------------------------------------------------------
 * Freeing / reference counting
 * -------------------------------------------------------------------------- */

void pmm_free_pages(uint32_t addr, unsigned order)
{
	uint32_t f = addr / PMM_FRAME_SIZE;
	if (f >= s_max_frames)
		return;

	if (mem_map[f].refcount == 0) {
		KLOG("pmm_free_pages: refcount underflow at frame ");
		KLOG_HEX(f);
		KLOG("\n");
		return;
	}

	/* Trust the descriptor's recorded order over the caller's argument so a
	 * mismatched free can't corrupt the buddy structure. */
	(void)order;
	if (--mem_map[f].refcount == 0)
		buddy_free(f, mem_map[f].order);
}

void pmm_free_frame(uint32_t addr)
{
	pmm_free_pages(addr, 0);
}

void pmm_inc_ref(uint32_t addr)
{
	uint32_t frame = addr / PMM_FRAME_SIZE;
	if (frame >= s_max_frames)
		return;
	if (mem_map[frame].refcount == 0) {
		KLOG("pmm_inc_ref: frame not allocated, refcount=0 at ");
		KLOG_HEX(frame);
		KLOG("\n");
		return;
	}
	if (mem_map[frame].refcount == 0xFF) {
		KLOG("pmm_inc_ref: refcount saturated at 255 for frame ");
		KLOG_HEX(frame);
		KLOG("\n");
		return;
	}
	mem_map[frame].refcount++;
}

uint8_t pmm_ref_count(uint32_t addr)
{
	uint32_t frame = addr / PMM_FRAME_SIZE;
	if (frame >= s_max_frames)
		return 0;
	return mem_map[frame].refcount;
}

/* --------------------------------------------------------------------------
 * Accounting / diagnostics
 * -------------------------------------------------------------------------- */

uint32_t pmm_managed_count(void)
{
	return s_total_managed_frames;
}

uint32_t pmm_free_count(void)
{
	return s_free_frames;
}

unsigned pmm_largest_free_order(void)
{
	for (unsigned o = PMM_MAX_ORDER; o-- > 0; )
		if (free_lists[o] != PMM_NIL)
			return o;
	return PMM_MAX_ORDER;   /* pool empty */
}
