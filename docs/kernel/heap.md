# heap - Kernel heap allocator

**Header:** `kernel/include/kernel/heap.h`  
**Source:** `kernel/arch/i386/heap.c`

Implements a simple first-fit linked-list allocator for the kernel heap.
The heap occupies the virtual address range **8 MiB – 24 MiB** (16 MiB
total), immediately above the identity-mapped boot window.

---

## Constants

| Constant | Value | Description |
|---|---|---|
| `HEAP_START` | `0x800000` (8 MiB) | First byte of the heap virtual address range. |
| `HEAP_MAX` | `0x1800000` (24 MiB) | Exclusive upper bound of the heap (16 MiB heap). |

---

## Block layout

Each allocation is preceded in memory by a `block_hdr_t`:

```
[ block_hdr_t | <user data> ] [ block_hdr_t | <user data> ] …
```

```c
typedef struct block_hdr {
    size_t            size;     // bytes of user data (not including header)
    uint32_t          is_free;  // 1 = free, 0 = in use
    struct block_hdr *next;     // next block in the list, or NULL
} block_hdr_t;
```

Splitting occurs only when the remainder would be at least 16 bytes of user
data (`SPLIT_MIN`), avoiding excessive fragmentation from tiny trailing
slivers.

---

## Functions

### `heap_init`

```c
void heap_init(void);
```

1. Call `paging_map_region(HEAP_START, HEAP_MAX - HEAP_START)` to identity-map
   the full 16 MiB heap region.
2. Place a single large free block covering the entire region.

Must be called after `paging_init()` and before any call to `kmalloc` or
`kfree`.

### `kmalloc`

```c
void *kmalloc(size_t size);
```

Allocate at least `size` bytes using a first-fit scan.  Splits the found
block if the remainder would be large enough.  Returns `NULL` if the heap is
exhausted or `size` is 0.

### `kfree`

```c
void kfree(void *ptr);
```

Mark the block at `ptr` as free and coalesce it with any immediately
following free blocks.  Passing `NULL` is a no-op.  Freeing a pointer that
was not returned by `kmalloc` or `krealloc` is undefined behaviour.

### `krealloc`

```c
void *krealloc(void *ptr, size_t size);
```

Resize an existing allocation:
- If `ptr` is `NULL`, behaves like `kmalloc(size)`.
- If `size` is 0, behaves like `kfree(ptr)` and returns `NULL`.
- If the current block is already large enough, returns `ptr` unchanged.
- Otherwise allocates a new block, copies the old data, and frees the old
  block.  Returns `NULL` on allocation failure without touching `ptr`.

### `heap_used` / `heap_free`

```c
size_t heap_used(void);
size_t heap_free(void);
```

Walk the block list and return the total bytes currently allocated or free,
respectively.  Useful for a future `meminfo` shell command.

---

## Future work

- Replace the first-fit scan with a segregated free-list or buddy system to
  reduce fragmentation and improve allocation speed.
- Add heap integrity checks (magic cookies, canaries) in debug builds.
- Implement a slab allocator for frequently allocated fixed-size objects
  (e.g. future process descriptors, file handles).
- Provide a `meminfo` system call / shell command using `heap_used()` and
  `heap_free()`.
