---
title: paging
parent: Kernel subsystems
grand_parent: Reference
---

# paging - Paging, page faults, and user virtual memory

**Header:** `kernel/include/kernel/paging.h`  
**Source:** `kernel/arch/i386/mm/paging.c`

Enables the x86 32-bit paging unit, maintains the early identity map used by
the kernel, and participates in the user virtual-memory model used by ELF
loading, `fork()` copy-on-write, and anonymous `mmap`.

The paging subsystem is split across several files:

| File | Role |
|---|---|
| `arch/i386/mm/paging.c` | Boot page directory, PSE identity map, CR0/CR3/CR4 setup. |
| `arch/i386/mm/vmm.c` | 4 KiB page-directory helpers for user address spaces and COW clones. |
| `arch/i386/mm/pmm.c` | Physical frame allocation and per-frame refcounts. |
| `arch/i386/debug/debug.c` | Page-fault diagnostics and COW fault resolution. |
| `arch/i386/proc/syscall.c` | `mmap2`/`munmap` syscall implementation. |

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

All pages in this range are supervisor-only and writable. User mappings are
created later as 4 KiB PTEs in per-task page directories.

This window covers:

- The kernel image (loaded at 1 MiB by GRUB).
- The VGA text buffer (`0xB8000`).
- The PMM bitmap, page directory, and other BSS/data.
- ACPI tables placed by firmware anywhere below 256 MiB.

---

## Kernel region mapping

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

## User address spaces

User tasks run in per-task page directories created by the VMM layer. The
kernel identity map remains present so kernel code can continue to access its
own text, data, heap, device buffers, and low physical memory after switching
CR3. User program pages are installed as 4 KiB user PTEs.

Important user mapping sources:

| Source | Mapping behavior |
|---|---|
| ELF loader | Maps PT_LOAD segments at the addresses requested by the executable. |
| User stack setup | Allocates and maps a ring-3 stack for process startup. |
| `fork()` | Clones the parent's address space with copy-on-write PTEs. |
| `mmap2(MAP_ANONYMOUS)` | Allocates zero-filled pages in a per-task anonymous-mmap window. |

Makar does not use a higher-half kernel layout. Kernel and user virtual
addresses share one 32-bit address space, and paging permissions enforce which
pages ring 3 may touch.

## Copy-on-write

`fork()` uses `vmm_clone_pd_cow(parent_pd)` instead of eagerly copying every
user page. Writable user PTEs in the parent and child are changed to:

- present;
- user-accessible;
- read-only;
- tagged with software bit `VMM_PTE_COW` (`0x200`);
- backed by the same physical frame with an incremented PMM refcount.

When either process writes to such a page, x86 raises a page fault because the
PTE is read-only. The fault path checks:

- the fault was a write;
- the PTE is present;
- the PTE has `VMM_PTE_COW`;
- the access belongs to a user mapping that the COW resolver understands.

If the frame refcount is 1, the resolver can clear the COW bit and restore
writability in place. If the refcount is greater than 1, it allocates a fresh
frame, copies the old page, maps the fresh frame writable for the faulting
process, and decrements the old frame's refcount.

`paging_init()` enables `CR0.WP` so supervisor writes also honour read-only PTEs.
That matters because tests and some kernel paths may write through user virtual
addresses while the page directory is active; without WP, kernel-mode writes
would bypass the COW fault and corrupt the shared frame.

## Anonymous mmap

`SYS_MMAP2` implements the Linux i386 syscall number `192` for anonymous
mappings. The current implementation is intentionally narrow:

- `MAP_ANONYMOUS` is supported;
- file-backed mappings are rejected;
- each mapping receives fresh zero-filled pages;
- protection is limited to the page flags Makar currently enforces;
- mappings are assigned from a per-task bump pointer starting at
  `USER_MMAP_BASE`;
- address reuse is not attempted when a mapping is unmapped.

`SYS_MUNMAP` unmaps the requested page range and frees the frames. It does not
rewind the task's `mmap_next` pointer, so a later anonymous mapping receives a
new virtual range even if the old range was freed.

This is enough for allocator tests and hosted-libc bring-up paths that need
anonymous heap-like memory, while keeping file mapping and demand paging out of
scope.

## Page-fault handler

The page-fault handler is installed on ISR 14. It reads the faulting address
from CR2, prints it alongside the error code when the fault cannot be resolved,
and calls `PANIC("Page fault")` for unhandled cases.

Before panicking, it gives the COW resolver a chance to handle write faults on
COW-tagged user pages. A successful COW resolution returns to the faulting
instruction, which then retries the write against a private writable page.

The error code bits indicate:
- Bit 0: 0 = not-present, 1 = protection violation.
- Bit 1: 0 = read, 1 = write.
- Bit 2: 0 = supervisor, 1 = user mode.

See the [Page Fault OSDev article](https://wiki.osdev.org/Exceptions#Page_Fault)
for the full error-code description.

---

## Current limitations and future work

- No file-backed `mmap`.
- No demand paging from disk or swap.
- No address-space layout randomisation.
- No guard pages around every kernel stack.
- No SMP TLB shootdown mechanism; Makar is currently uniprocessor.
- No higher-half kernel split.

Those are deliberate constraints, not missing pieces for the current hosted
libc milestone. The active baseline is per-task address spaces, COW `fork()`,
anonymous `mmap2`, and deterministic in-guest tests for those mechanisms.
