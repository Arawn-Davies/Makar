# pmm - Physical memory manager

**Header:** `kernel/include/kernel/pmm.h`  
**Source:** `kernel/arch/i386/pmm.c`

Manages the pool of available physical RAM using a flat bitmap.  Each bit
represents one 4 KiB frame: `1` = used, `0` = free.

---

## Constants

| Constant | Value | Description |
|---|---|---|
| `PMM_FRAME_SIZE` | `0x1000` (4096) | Size of one physical frame in bytes. |
| `PMM_ALLOC_ERROR` | `0xFFFFFFFF` | Sentinel returned by `pmm_alloc_frame()` when no free frame is available. |

---

## Initialisation (`pmm_init`)

```c
void pmm_init(uint32_t magic, multiboot2_info_t *mbi);
```

Build the frame bitmap from the Multiboot 2 memory map tag.

| Parameter | Description |
|---|---|
| `magic` | Multiboot 2 magic value passed by the bootloader (`0x36D76289`). If wrong, no memory is freed. |
| `mbi` | Pointer to the Multiboot 2 information structure. |

**Algorithm:**

1. Mark every frame as used (`memset(bitmap, 0xFF, ...)`).
2. Validate the Multiboot 2 magic; bail out if incorrect.
3. Walk the tag list to find the memory map tag (type 6).
4. For each `MULTIBOOT2_MEMORY_AVAILABLE` region, round the region boundaries
   to 4 KiB and clear the corresponding bitmap bits (free).
5. Re-mark frame 0 (physical address `0x0000–0x0FFF`) as used - guards
   against null-pointer dereferences.
6. Re-mark all frames occupied by the kernel image (loaded at 1 MiB) as used,
   using the `_kernel_end` symbol from the linker script as the upper bound.
7. Print the free frame count and total free MiB to both the VGA terminal and
   the serial port.

---

## Functions

### `pmm_alloc_frame`

```c
uint32_t pmm_alloc_frame(void);
```

Allocate one physical frame using a linear scan of the bitmap.  Returns the
**physical address** of the allocated frame (always a multiple of
`PMM_FRAME_SIZE`), or `PMM_ALLOC_ERROR` if all frames are in use.

The frame is marked used before the function returns, so concurrent callers
(once interrupts are involved) must ensure mutual exclusion at a higher level.

### `pmm_free_frame`

```c
void pmm_free_frame(uint32_t addr);
```

Return the frame at physical address `addr` to the free pool.  `addr` must be
a value previously returned by `pmm_alloc_frame()`; passing an arbitrary
address will silently corrupt the bitmap.

### `pmm_free_count`

```c
uint32_t pmm_free_count(void);
```

Count and return the number of frames currently in the free pool, using
Brian Kernighan's bit-counting trick on each 32-bit bitmap word.

---

## Bitmap internals

The bitmap covers the entire 32-bit physical address space:

- **4 GiB / 4 KiB = 1 048 576 frames**
- **1 048 576 / 32 = 32 768 `uint32_t` words** → 128 KiB of BSS

The bitmap is statically allocated in BSS and is therefore zeroed by the
runtime startup before `kernel_main` runs.  `pmm_init` immediately fills it
with `0xFF` (all used) before selectively freeing usable regions.

---

## Future work

- Break the 4GB barrier! This isn't the 2000's with a Pentium 4 anymore
- Add a multi-frame contiguous allocator (`pmm_alloc_contiguous(n)`) for DMA
  buffers and page-table pools.
- Expose NUMA zone information if a multi-processor platform is targeted.
- Track total/used/free frame statistics without a full scan in
  `pmm_free_count`.
