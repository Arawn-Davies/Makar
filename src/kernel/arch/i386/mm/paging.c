#include <kernel/paging.h>
#include <kernel/isr.h>
#include <kernel/tty.h>
#include <kernel/system.h>
#include <kernel/serial.h>

/* ---------------------------------------------------------------------------
 * Identity map: 4 MiB PSE large pages covering the first 256 MiB.
 *
 * Why large pages?
 *   • Each PDE entry directly maps 4 MiB – no intermediate page table needed.
 *   • 64 entries cover 256 MiB, enough for ACPI tables placed anywhere in
 *     low physical memory by firmware.
 *   • This mirrors the large-page strategy used by 64-bit kernels (which use
 *     2 MiB pages in long mode); switching to long mode later only requires
 *     rebuilding the page structures, not changing the overall design.
 *
 * Addresses above 256 MiB (e.g. VESA framebuffers) are still handled on
 * demand by paging_map_region() using fine-grained 4 KiB pages.
 *
 * OSDev references:
 *   Paging (32-bit)           – https://wiki.osdev.org/Paging
 *   Page Size Extensions      – https://wiki.osdev.org/Page_Size_Extension
 *   Control Register 4 (CR4) – https://wiki.osdev.org/CPU_Registers_x86#CR4
 *   Page fault / error codes  – https://wiki.osdev.org/Exceptions#Page_Fault
 * ------------------------------------------------------------------------- */
#define IDENTITY_MAP_MB      256u
#define LARGE_PAGE_SIZE      (4u * 1024u * 1024u)   /* 4 MiB per PSE entry */
#define IDENTITY_LARGE_PAGES (IDENTITY_MAP_MB * 1024u * 1024u / LARGE_PAGE_SIZE) /* 64 */

/* Pool of extra 4 KiB page tables for paging_map_region() (addresses above
   the large-page identity window).  32 tables × 1024 entries × 4 KiB = 128 MiB
   of additional mappable virtual address space.                              */
#define EXTRA_PAGE_TABLES 32

/* Page-entry flags */
#define PAGE_PRESENT   0x1u
#define PAGE_WRITABLE  0x2u
#define PAGE_LARGE     0x80u  /* PS bit: 4 MiB page (requires CR4.PSE) */

/* Static, page-aligned structures.  All live inside the kernel image which
   is itself within the 0–256 MiB identity-mapped window.                    */
static uint32_t page_directory[1024]                              __attribute__((aligned(4096)));

uint32_t *paging_kernel_pd(void) { return page_directory; }
static uint32_t extra_page_tables[EXTRA_PAGE_TABLES][1024]       __attribute__((aligned(4096)));
static uint32_t next_extra_pt = 0;

/* ISR 14 – Page-fault handler.
   CR2 holds the linear address that caused the fault. */
static void page_fault_handler(registers_t *regs)
{
    uint32_t faulting_address;
    asm volatile("mov %%cr2, %0" : "=r"(faulting_address));

    t_writestring("Page fault at 0x");
    t_hex(faulting_address);
    t_writestring(" (err=0x");
    t_hex(regs->err_code);
    t_writestring(")\n");

    PANIC("Page fault");
}

void paging_init(void)
{
    /* Enable CR4.PSE so the processor honours the PS bit in PDE entries,
       turning them into 4 MiB large pages.  This is the 32-bit equivalent
       of the 2 MiB large pages used by x86-64 long-mode kernels.          */
    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 4);   /* PSE – Page Size Extensions */
    asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    /* Identity-map 0–256 MiB: one PDE per 4 MiB region, PS bit set.
       No intermediate page table is needed for these entries.              */
    for (uint32_t i = 0; i < IDENTITY_LARGE_PAGES; i++) {
        uint32_t phys = i * LARGE_PAGE_SIZE;
        page_directory[i] = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE;
    }

    /* Load CR3 with the physical address of the page directory. */
    asm volatile("mov %0, %%cr3" :: "r"(page_directory) : "memory");

    /* Enable paging: set CR0.PG (bit 31). */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    t_writestring("Paging: enabled (identity-mapped 0-256 MiB, 4 MiB large pages)\n");
    KLOG("paging_init: 256 MiB identity map via PSE large pages\n");
}

void paging_map_region(uint32_t phys_start, uint32_t size)
{
    if (size == 0)
        return;

    KLOG("paging_map_region: ");
    KLOG_HEX(phys_start);
    KLOG(" len=");
    KLOG_HEX(size);
    KLOG("\n");

    /* Work with page-aligned boundaries. */
    uint32_t start = phys_start & ~0xFFFu;
    /* Guard against overflow: clamp to the last page-aligned address. */
    uint32_t end;
    if (size > 0xFFFFFFFFu - phys_start)
        end = 0xFFFFF000u;
    else
        end = (phys_start + size + 0xFFFu) & ~0xFFFu;

    if (end <= start)
        return;

    for (uint32_t addr = start; addr != end; addr += 0x1000) {
        uint32_t pdi = addr >> 22;             /* page-directory index  */
        uint32_t pti = (addr >> 12) & 0x3FFu; /* page-table index      */

        /* If this PDE is a large-page entry the 4 MiB region is already
           identity-mapped; nothing to do for any page within it.          */
        if (page_directory[pdi] & PAGE_LARGE)
            continue;

        /* Allocate a fresh 4 KiB page table for this directory slot if needed. */
        if (!(page_directory[pdi] & PAGE_PRESENT)) {
            if (next_extra_pt >= EXTRA_PAGE_TABLES)
                return; /* pool exhausted – give up */

            uint32_t *pt = extra_page_tables[next_extra_pt++];

            /* Zero the new page table (BSS is zeroed, but be explicit). */
            for (uint32_t i = 0; i < 1024; i++)
                pt[i] = 0;

            page_directory[pdi] = (uint32_t)pt | PAGE_PRESENT | PAGE_WRITABLE;
        }

        /* Map the 4 KiB page if it is not already present. */
        uint32_t *pt = (uint32_t *)(page_directory[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT))
            pt[pti] = addr | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Flush the TLB by reloading CR3. */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

