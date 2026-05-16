---
title: paging
parent: Kernel subsystems
---

# paging - Paging and virtual memory

**Header:** `kernel/include/kernel/paging.h`  
**Source:** `kernel/arch/i386/mm/paging.c`

Enables the x86 32-bit paging unit and provides a runtime region-mapping
function used by the heap and VESA framebuffer drivers to access memory
outside the initial boot window.

**OSDev references:**
- [Paging (32-bit)](https://wiki.osdev.org/Paging)
- [Page Size Extension (PSE)](https://wiki.osdev.org/Page_Size_Extension)
- [CR4 control register](https://wiki.osdev.org/CPU_Registers_x86#CR4)
- [Page Fault exception and error code](https://wiki.osdev.org/Exceptions#Page_Fault)

---

## Boot-time identity map

`paging_init()` identity-maps the first **256 MiB** of physical memory
(physical address = virtual address) using **4 MiB PSE large pages**.

Before loading CR3, `CR4.PSE` (bit 4) is set so the processor honours the
PS bit in PDE entries.  Each of the 64 large-page PDE entries maps one
aligned 4 MiB region directly - no intermediate page table is needed.
This is the 32-bit equivalent of the 2 MiB large pages used by x86-64
long-mode kernels.

All pages in this range are supervisor-only and writable.

This window covers:

- The kernel image (loaded at 1 MiB by GRUB).
- The VGA text buffer (`0xB8000`).
- The PMM bitmap, page directory, and other BSS/data.
- ACPI tables placed by firmware anywhere below 256 MiB.

---

## Page-table pool

A static pool of 32 extra 4 KiB page tables (`extra_page_tables`) handles
post-boot mapping requests for addresses **above 256 MiB**.  Each page
table covers 4 MiB of virtual address space, so the pool can map up to
**128 MiB** of additional regions.  This is enough for the 16 MiB kernel
heap and the typical VESA framebuffer.

Requests that fall within the 256 MiB large-page window are detected by
checking the `PAGE_LARGE` flag in the relevant PDE and silently skipped.

---

## Functions

### `paging_init`

```c
void paging_init(void);
```

1. Set `CR4.PSE` (bit 4) to enable 4 MiB large pages.
2. Fill 64 PDE entries with identity-map entries for 0–256 MiB (PS bit set).
3. Register `page_fault_handler` on ISR 14.
4. Load CR3 with the physical address of the page directory.
5. Set CR0 bit 31 (PG) to enable paging.

### `paging_map_region`

```c
void paging_map_region(uint32_t phys_start, uint32_t size);
```

Identity-map the physical address range `[phys_start, phys_start + size)`.
Pages that fall within a large-page PDE entry (already covered by the
256 MiB boot window) are silently skipped.  If the page-table pool is
exhausted the function returns without mapping anything further.

| Parameter | Description |
|---|---|
| `phys_start` | Physical start address. Need not be page-aligned; the function rounds down. |
| `size` | Length in bytes. Need not be page-aligned; the function rounds up. |

Called by:
- `heap_init()` - to map the 16 MiB heap region.
- `vesa_tty_init()` - to map the VESA linear framebuffer before the first pixel write.
- `acpi_init()` (via `acpi_map_table()`) - to map RSDT, FADT, and DSDT.

---

## Page-fault handler

A simple panic handler is installed on ISR 14.  It reads the faulting address
from CR2, prints it alongside the error code, then calls `PANIC("Page fault")`.

The error code bits indicate:
- Bit 0: 0 = not-present, 1 = protection violation.
- Bit 1: 0 = read, 1 = write.
- Bit 2: 0 = supervisor, 1 = user mode.

See the [Page Fault OSDev article](https://wiki.osdev.org/Exceptions#Page_Fault)
for the full error-code description.

---

## Future work

- Separate kernel and user address spaces (higher-half kernel or full
  split-address mapping).
- Demand paging: swap absent pages in from a backing store.
- Copy-on-write support for future process forking.
- A proper TLB shootdown mechanism for SMP.
- Guard pages around the kernel stack to catch stack overflows early.
