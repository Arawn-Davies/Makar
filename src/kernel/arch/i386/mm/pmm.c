#include <kernel/pmm.h>
#include <kernel/tty.h>
#include <kernel/serial.h>
#include <string.h>

/* 4 GB / 4 KB = 1 048 576 frames, stored as 32 768 32-bit words. */
#define PMM_MAX_FRAMES   (0x100000U)
#define PMM_BITMAP_WORDS (PMM_MAX_FRAMES / 32)

/* bit = 1 → frame used   bit = 0 → frame free */
static uint32_t bitmap[PMM_BITMAP_WORDS];

/* Total managed (= bootloader-available, minus null page + kernel image)
 * frames.  Constant after pmm_init.  Used by /proc/meminfo to expose
 * MemTotal so userspace tools (maktop) can show a memory bar. */
static uint32_t s_total_managed_frames;

static inline void pmm_set_frame(uint32_t frame)
{
	bitmap[frame / 32] |= (1U << (frame % 32));
}

static inline void pmm_clear_frame(uint32_t frame)
{
	bitmap[frame / 32] &= ~(1U << (frame % 32));
}

void pmm_init(uint32_t magic, multiboot2_info_t *mbi)
{
	extern uint32_t _kernel_end;

	/* Start with every frame marked as used. */
	memset(bitmap, 0xFF, sizeof(bitmap));

	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
		t_writestring("PMM: invalid multiboot2 magic, no memory freed\n");
		KLOG("pmm_init: invalid multiboot2 magic\n");
		return;
	}

	/* Walk the Multiboot 2 tag list.  Tags start immediately after the
	   8-byte info header and are each padded to an 8-byte boundary. */
	multiboot2_tag_mmap_t *mmap_tag = NULL;

	uint8_t *tag_ptr = (uint8_t *)mbi + sizeof(multiboot2_info_t);
	uint8_t *info_end = (uint8_t *)mbi + mbi->total_size;

	while (tag_ptr < info_end) {
		multiboot2_tag_t *tag = (multiboot2_tag_t *)tag_ptr;

		if (tag->type == MULTIBOOT2_TAG_TYPE_END)
			break;

		if (tag->type == MULTIBOOT2_TAG_TYPE_MMAP) {
			mmap_tag = (multiboot2_tag_mmap_t *)tag;
			break;
		}

		/* Advance to next tag, aligned to 8 bytes. */
		tag_ptr += (tag->size + 7) & ~7u;
	}

	if (!mmap_tag) {
		t_writestring("PMM: no memory map tag from bootloader, no memory freed\n");
		KLOG("pmm_init: no memory map tag\n");
		return;
	}

	/* Walk memory map entries. */
	uint8_t *entry_ptr = (uint8_t *)mmap_tag + sizeof(multiboot2_tag_mmap_t);
	uint8_t *mmap_end  = (uint8_t *)mmap_tag + mmap_tag->size;

	while (entry_ptr < mmap_end) {
		multiboot2_mmap_entry_t *entry = (multiboot2_mmap_entry_t *)entry_ptr;

		if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE) {
			/* Round start up and end down to 4 KiB boundaries. */
			uint64_t region_start =
				(entry->base_addr + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
			uint64_t region_end =
				(entry->base_addr + entry->length) & ~(uint64_t)(PMM_FRAME_SIZE - 1);

			for (uint64_t addr = region_start; addr < region_end; addr += PMM_FRAME_SIZE) {
				if (addr < (uint64_t)PMM_MAX_FRAMES * PMM_FRAME_SIZE)
					pmm_clear_frame((uint32_t)(addr / PMM_FRAME_SIZE));
			}
		}

		entry_ptr += mmap_tag->entry_size;
	}

	/* Re-mark the null page as used (guard against NULL dereferences). */
	pmm_set_frame(0);

	/* Re-mark all frames occupied by the kernel image as used.
	   The kernel is loaded at 1 MiB; _kernel_end is the first address
	   beyond the last kernel section, as defined in linker.ld. */
	uint32_t kstart = 0x100000 / PMM_FRAME_SIZE;
	uint32_t kend   = ((uint32_t)&_kernel_end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
	for (uint32_t f = kstart; f < kend; f++)
		pmm_set_frame(f);

	/* Capture the pool size before any runtime allocations; this is
	 * what /proc/meminfo's MemTotal reports.  pmm_free_count() walks
	 * the bitmap so we cache the result rather than re-counting. */
	uint32_t free_frames = pmm_free_count();
	s_total_managed_frames = free_frames;
	t_writestring("PMM: ");
	t_dec(free_frames);
	t_writestring(" frames free (");
	t_dec((free_frames * PMM_FRAME_SIZE) / (1024 * 1024));
	t_writestring(" MiB)\n");

	KLOG("pmm_init: ");
	KLOG_DEC(free_frames);
	KLOG(" frames free (");
	KLOG_DEC((free_frames * PMM_FRAME_SIZE) / (1024 * 1024));
	KLOG(" MiB)\n");
}

uint32_t pmm_alloc_frame(void)
{
	for (uint32_t i = 0; i < PMM_BITMAP_WORDS; i++) {
		if (bitmap[i] == 0xFFFFFFFF)
			continue;  /* all 32 frames in this word are used */
		for (uint32_t bit = 0; bit < 32; bit++) {
			if (!((bitmap[i] >> bit) & 1)) {
				uint32_t frame = i * 32 + bit;
				pmm_set_frame(frame);
				return frame * PMM_FRAME_SIZE;
			}
		}
	}
	return PMM_ALLOC_ERROR;  /* out of memory */
}

void pmm_free_frame(uint32_t addr)
{
	pmm_clear_frame(addr / PMM_FRAME_SIZE);
}

uint32_t pmm_managed_count(void)
{
	return s_total_managed_frames;
}

uint32_t pmm_free_count(void)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < PMM_BITMAP_WORDS; i++) {
		uint32_t word = ~bitmap[i];  /* flip bits: 1 now means free */
		/* Brian Kernighan's bit-count */
		while (word) {
			count++;
			word &= word - 1;
		}
	}
	return count;
}
