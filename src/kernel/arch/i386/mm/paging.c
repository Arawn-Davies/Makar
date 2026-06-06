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

/* Higher-half kernel virtual base (must match KERNEL_VBASE in linker.ld and
 * boot.S).  The kernel image and all its static structures (page_directory[],
 * extra_page_tables[], mem_map[], ...) are linked at KERNEL_VBASE + phys, but
 * the low physical region is also permanently identity-mapped (the boot stub
 * sets up PDE[0..3] and paging_init() extends the identity window to 256 MiB).
 *
 * CR3 and PDE entries need PHYSICAL addresses; a kernel static address minus
 * KERNEL_VBASE yields its physical address. */
/* The in-OS TCC rebuild (build-kernel-tcc.sh / rebuild-kernel.sh) has no GNU
 * ld script and links the kernel low-half (identity).  In that case KERNEL_VBASE
 * is 0 so V2P() is the identity and the "high" PD window coincides with the
 * low one -- the kernel stays at its physical address.  The gcc build links
 * higher-half at 0xC0000000 (linker.ld + boot.S). */
#ifdef __TINYC__
#define KERNEL_VBASE   0x00000000u
#else
#define KERNEL_VBASE   0xC0000000u
#endif
#define V2P(x)         ((uint32_t)(x) - KERNEL_VBASE)

/* First high page-directory index (0xC0000000 >> 22 = 768; 0 under TCC). */
#define KERNEL_PD_IDX  (KERNEL_VBASE >> 22)

/* Page-entry flags */
#define PAGE_PRESENT   0x1u
#define PAGE_WRITABLE  0x2u
#define PAGE_PWT       0x8u   /* page-level write-through (PAT index bit 0)   */
#define PAGE_PCD       0x10u  /* page-level cache disable  (PAT index bit 1)  */
#define PAGE_LARGE     0x80u  /* PS bit: 4 MiB page (requires CR4.PSE)        */
#define PAGE_PAT       0x80u  /* in a 4 KiB *PTE*, bit 7 is the PAT index MSB */

/* Set once by paging_init() if the CPU has PAT and we armed a write-combining
 * slot.  Until then paging_map_region_wc() falls back to plain mappings. */
static uint32_t s_pat_wc_ok = 0;

/* Set once a WC MTRR has been pinned over the framebuffer (see
 * paging_set_mtrr_wc); guards against programming a second overlapping slot. */
static uint32_t s_mtrr_fb_done = 0;

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

/* Arm a write-combining (WC) memory type in the PAT so framebuffer pages can
 * be mapped WC instead of inheriting the firmware's uncacheable (UC) MTRR for
 * the PCI MMIO hole.  Under TCG (QEMU) MMIO is cheap so UC is invisible; under
 * a real VT-x hypervisor (VirtualBox, Hyper-V) or bare metal, UC framebuffer
 * writes trap per access and make the GUI crawl.  Per the Intel SDM, a WC PAT
 * type yields WC even where the MTRRs say UC, so this works without touching
 * MTRRs.  Leaves PA0..PA3 at their power-on defaults (WB,WT,UC-,UC) so ordinary
 * write-back mappings are unaffected; only PA4 (selected by PTE PAT bit, with
 * PCD=PWT=0) is reprogrammed to WC. */
static void paging_init_pat(void)
{
    /* CPUID.01h:EDX[16] = PAT supported. */
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1u));
    if (!(edx & (1u << 16)))
        return;                       /* no PAT: leave WC disabled */

    /* IA32_PAT = MSR 0x277.  PA4 is byte 4 -> low byte of the high dword. */
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277u));
    hi = (hi & 0xFFFFFF00u) | 0x01u;  /* PA4 = WC (memory type 0x01) */
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x277u));

    s_pat_wc_ok = 1;
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

    /* Build the runtime page directory.  Two windows, both via 4 MiB PSE
       large pages, both pointing at the same low physical memory:
         • Identity window  0x00000000..0x10000000 (PDE 0..63)   -> phys 0..256M
         • Higher-half      0xC0000000..0xD0000000 (PDE 768..831)-> phys 0..256M
       The kernel runs at the higher-half addresses; the identity window is kept
       permanently so the PMM/heap/VMM can dereference low physical frames as
       (uint32_t *)phys, the Multiboot info pointer (a low physical address in
       EBX) stays reachable, and the VGA buffer / ACPI tables map phys==virt.   */
    for (uint32_t i = 0; i < IDENTITY_LARGE_PAGES; i++) {
        uint32_t phys = i * LARGE_PAGE_SIZE;
        uint32_t pde  = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE;
        page_directory[i]                 = pde;   /* identity  */
        page_directory[KERNEL_PD_IDX + i] = pde;   /* higher-half */
    }

    /* Load CR3 with the PHYSICAL address of the page directory (it is a kernel
       static, so its linked address is high; subtract KERNEL_VBASE).  This
       replaces the minimal boot page directory built in boot.S.              */
    asm volatile("mov %0, %%cr3" :: "r"(V2P(page_directory)) : "memory");

    /* Enable paging and write-protect:
     *   bit 31 (PG): turn paging on.
     *   bit 16 (WP): respect user-page R/W bits even from ring 0.
     *
     * WP is required for fork+COW (slice 12c) -- without it, kernel
     * writes to a COW-tagged user page silently succeed instead of
     * faulting into the COW handler.  Linux/ELKS both enable WP for
     * the same reason. */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u;
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    paging_init_pat();

    t_writestring("Paging: higher-half (0xC0000000->0, +256 MiB identity, 4 MiB pages, WP on)\n");
    KLOG("paging_init: higher-half kernel @ 0xC0000000, 256 MiB identity + high map, CR0.WP=1\n");
}

static void map_region_flags(uint32_t phys_start, uint32_t size, uint32_t extra_flags)
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

            /* PDE stores the PHYSICAL address of the page table.  extra_page_tables
               is a kernel static (high virtual); subtract KERNEL_VBASE.          */
            page_directory[pdi] = V2P(pt) | PAGE_PRESENT | PAGE_WRITABLE;
        }

        /* Map the 4 KiB page if it is not already present.  The PDE holds a
           physical PT address; dereference it through the low identity map. */
        uint32_t *pt = (uint32_t *)(page_directory[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT))
            pt[pti] = addr | PAGE_PRESENT | PAGE_WRITABLE | extra_flags;
    }

    /* Flush the TLB by reloading CR3. */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void paging_map_region(uint32_t phys_start, uint32_t size)
{
    map_region_flags(phys_start, size, 0);
}

/* Like paging_map_region(), but maps the range write-combining when the PAT
 * WC slot was armed at init (falls back to a plain mapping otherwise).  Intended
 * for the linear framebuffer: WC batches the many small pixel writes that would
 * otherwise each trap as UC MMIO on VT-x hypervisors / real hardware. */
/*
 * paging_set_mtrr_wc – pin a write-combining variable-range MTRR over
 * [base, base+size).
 *
 * The framebuffer lives in the PCI MMIO hole, which firmware marks UC (via the
 * default MTRR type) on VT-x hypervisors (VMware, Hyper-V, VirtualBox) and bare
 * metal.  PAT-WC alone CANNOT override a UC MTRR -- per the Intel SDM memory-
 * type combining rules, PAT=WC + MTRR=UC still resolves to UC -- so without a WC
 * MTRR every pixel write traps as an uncached MMIO transaction and the GUI
 * crawls (the symptom: smooth under QEMU/TCG, where MMIO is cheap, but sluggish
 * under real virtualization).  A *variable* MTRR takes precedence over the
 * default type and doesn't overlap any variable UC MTRR (the hole's UC comes
 * from the default type), so the WC range is well-defined.  This is exactly what
 * Linux's framebuffer/DRM drivers do via arch_phys_wc_add().
 *
 * Returns 1 if a WC MTRR was armed, 0 otherwise (no MTRR support, no free slot,
 * or the range can't be expressed as one naturally-aligned power-of-two block).
 */
static int paging_set_mtrr_wc(uint32_t base, uint32_t size)
{
    uint32_t eax, ebx, ecx, edx;

    /* CPUID.01h:EDX[12] = MTRR supported. */
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u));
    if (!(edx & (1u << 12)))
        return 0;

    /* A variable MTRR covers one naturally-aligned power-of-two block.  Round
     * the span up to a power of two; framebuffer bases on real adapters and
     * hypervisors are aligned to large powers of two, so the alignment holds. */
    uint32_t pw = 0x1000u;                       /* 4 KiB minimum */
    while (pw < size && pw < 0x40000000u) pw <<= 1;
    if (pw < size)            return 0;          /* span too large to cover */
    if (base & (pw - 1u))     return 0;          /* base not aligned to pw  */

    /* IA32_MTRRCAP[7:0] = number of variable MTRR pairs. */
    uint32_t cap_lo, cap_hi;
    asm volatile("rdmsr" : "=a"(cap_lo), "=d"(cap_hi) : "c"(0xFEu));
    uint32_t vcnt = cap_lo & 0xFFu;
    if (vcnt == 0)
        return 0;

    /* Physical address width (CPUID 0x80000008:EAX[7:0]); default to 36. */
    uint32_t phys_bits = 36;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u));
    if (eax >= 0x80000008u) {
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000008u));
        phys_bits = eax & 0xFFu;
        if (phys_bits < 32u || phys_bits > 52u) phys_bits = 36u;
    }

    /* Find a free variable MTRR pair (PHYSMASK valid bit, bit 11, clear). */
    int slot = -1;
    for (uint32_t i = 0; i < vcnt; i++) {
        uint32_t m_lo, m_hi;
        asm volatile("rdmsr" : "=a"(m_lo), "=d"(m_hi) : "c"(0x201u + 2u * i));
        if (!(m_lo & (1u << 11))) { slot = (int)i; break; }
    }
    if (slot < 0)
        return 0;

    /* PHYSBASE: base (4 KiB-aligned) | memory type 0x01 (WC).  base < 4 GiB, so
     * the high dword is 0.  PHYSMASK: mask of significant address bits | valid. */
    uint32_t base_lo = (base & 0xFFFFF000u) | 0x01u;
    uint32_t base_hi = 0;
    uint32_t mask_lo = ((~(pw - 1u)) & 0xFFFFF000u) | (1u << 11);
    uint32_t mask_hi = (phys_bits > 32u) ? ((1u << (phys_bits - 32u)) - 1u) : 0u;
    uint32_t msr     = 0x200u + 2u * (uint32_t)slot;

    /* --- Canonical MTRR-change sequence (Intel SDM Vol 3, §12.11.7.2). --- */
    uint32_t flags;
    asm volatile("pushf; pop %0" : "=r"(flags));
    asm volatile("cli");

    /* Clear CR4.PGE (if set) so global TLB entries are flushed by the CR3
     * reloads below; restored at the end. */
    uint32_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1u << 7))
        asm volatile("mov %0, %%cr4" :: "r"(cr4 & ~(1u << 7)) : "memory");

    /* Enter no-fill cache mode (CR0.CD=1, NW=0) and flush caches + TLB. */
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" :: "r"((cr0 | (1u << 30)) & ~(1u << 29)) : "memory");
    asm volatile("wbinvd");
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");

    /* Disable MTRRs (clear E, bit 11, in IA32_MTRR_DEF_TYPE = 0x2FF). */
    uint32_t def_lo, def_hi;
    asm volatile("rdmsr" : "=a"(def_lo), "=d"(def_hi) : "c"(0x2FFu));
    asm volatile("wrmsr" :: "a"(def_lo & ~(1u << 11)), "d"(def_hi), "c"(0x2FFu));

    /* Program the chosen variable MTRR pair. */
    asm volatile("wrmsr" :: "a"(base_lo), "d"(base_hi), "c"(msr));
    asm volatile("wrmsr" :: "a"(mask_lo), "d"(mask_hi), "c"(msr + 1u));

    /* Re-enable MTRRs (set E). */
    asm volatile("wrmsr" :: "a"(def_lo | (1u << 11)), "d"(def_hi), "c"(0x2FFu));

    /* Flush caches + TLB again, then leave no-fill mode. */
    asm volatile("wbinvd");
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    /* Restore CR4.PGE and the interrupt flag. */
    if (cr4 & (1u << 7))
        asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    if (flags & (1u << 9))
        asm volatile("sti");

    return 1;
}

void paging_map_region_wc(uint32_t phys_start, uint32_t size)
{
    map_region_flags(phys_start, size, s_pat_wc_ok ? PAGE_PAT : 0u);

    /* Also pin a WC MTRR over the framebuffer so write-combining survives a UC
     * default MTRR type (PAT-WC cannot override that on its own).  Once only --
     * the FB base is fixed, and a second overlapping slot would waste an MTRR. */
    if (!s_mtrr_fb_done && size) {
        paging_set_mtrr_wc(phys_start, size);
        s_mtrr_fb_done = 1;
    }
}

