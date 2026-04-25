/*
 * ktest.c -- In-kernel unit test runner.
 *
 * Each suite is a static function called from ktest_run_all().  Results are
 * written directly to the VGA terminal so they work without heap or FS.
 */

#include <kernel/ktest.h>
#include <kernel/acpi.h>
#include <kernel/partition.h>
#include <kernel/pmm.h>
#include <kernel/heap.h>
#include <kernel/vmm.h>
#include <kernel/paging.h>
#include <kernel/descr_tbl.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Harness state & primitives
 * ------------------------------------------------------------------------- */

int ktest_pass_count = 0;
int ktest_fail_count = 0;

void ktest_begin(const char *suite)
{
    ktest_pass_count = 0;
    ktest_fail_count = 0;
    t_writestring("\n[ktest] suite: ");
    t_writestring(suite);
    t_putchar('\n');
}

void ktest_assert(int cond, const char *expr, const char *file, uint32_t line)
{
    if (cond) {
        t_writestring("  PASS: ");
        t_writestring(expr);
        t_putchar('\n');
        ktest_pass_count++;
    } else {
        t_writestring("  FAIL: ");
        t_writestring(expr);
        t_writestring("  (");
        t_writestring(file);
        t_putchar(':');
        t_dec(line);
        t_writestring(")\n");
        ktest_fail_count++;
    }
}

void ktest_summary(void)
{
    t_writestring("[ktest] results: ");
    t_dec((uint32_t)ktest_pass_count);
    t_writestring(" passed, ");
    t_dec((uint32_t)ktest_fail_count);
    t_writestring(" failed\n");
}

/* ---------------------------------------------------------------------------
 * Suite: ACPI helpers
 *
 * acpi_checksum() is the only internal helper we can call from outside
 * acpi.c without touching real hardware.  We test it with known buffers.
 * ------------------------------------------------------------------------- */

static void test_acpi_checksum(void)
{
    ktest_begin("acpi_checksum");

    /* A buffer whose byte sum is 0 — valid. */
    uint8_t good[4] = {0x01, 0x02, 0x03, 0xFA}; /* 1+2+3+250 = 256 → 0 mod 256 */
    KTEST_ASSERT(acpi_checksum(good, 4));

    /* Off-by-one: change the last byte so the sum is non-zero. */
    uint8_t bad[4] = {0x01, 0x02, 0x03, 0xFB};
    KTEST_ASSERT(!acpi_checksum(bad, 4));

    /* Single byte whose value is 0 — valid (sum = 0). */
    uint8_t zero[1] = {0x00};
    KTEST_ASSERT(acpi_checksum(zero, 1));

    /* Single byte whose value is non-zero — invalid. */
    uint8_t nonzero[1] = {0x01};
    KTEST_ASSERT(!acpi_checksum(nonzero, 1));

    /* Empty buffer (length 0) — sum is 0, always valid. */
    KTEST_ASSERT(acpi_checksum(good, 0));

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: string helpers (sanity-check the libc stubs used by the kernel)
 * ------------------------------------------------------------------------- */

static void test_string(void)
{
    ktest_begin("string");

    KTEST_ASSERT(strlen("hello") == 5);
    KTEST_ASSERT(strlen("") == 0);

    KTEST_ASSERT(strcmp("abc", "abc") == 0);
    KTEST_ASSERT(strcmp("abc", "abd") < 0);
    KTEST_ASSERT(strcmp("abd", "abc") > 0);

    KTEST_ASSERT(strncmp("abcX", "abcY", 3) == 0);
    KTEST_ASSERT(strncmp("abcX", "abcY", 4) != 0);

    char buf[16];
    memset(buf, 0xAA, sizeof(buf));
    KTEST_ASSERT((uint8_t)buf[0] == 0xAA);

    memcpy(buf, "hello", 6);
    KTEST_ASSERT(strcmp(buf, "hello") == 0);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: partition helpers
 *
 * Tests the pure-logic helpers in partition.c that can run without hardware.
 * part_type_name() and part_guid_type_name() both look up tables in BSS,
 * so they work without any disk being present.
 * ------------------------------------------------------------------------- */

static void test_partition(void)
{
    ktest_begin("partition");

    /* MBR type name lookup */
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_EMPTY),     "Empty")          == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_FAT32_LBA), "FAT32 (LBA)")    == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_FAT32_CHS), "FAT32 (CHS)")    == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_GPT_PROT),  "GPT protective") == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_EFI),       "EFI System")     == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_MDFS),      "MDFS")           == 0);
    KTEST_ASSERT(strcmp(part_type_name(PART_MBR_LINUX),     "Linux")          == 0);
    /* Unknown type returns a non-empty string (not NULL). */
    KTEST_ASSERT(part_type_name(0xAB) != 0);

    /* GPT GUID type name lookup */
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_FAT32), "FAT32")      == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_EFI),   "EFI System") == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_LINUX), "Linux Data") == 0);
    KTEST_ASSERT(strcmp(part_guid_type_name(PART_GUID_MDFS),  "MDFS")       == 0);

    /* All-zero GUID is "Unused". */
    static const uint8_t zero16[16] = {0};
    KTEST_ASSERT(strcmp(part_guid_type_name(zero16), "Unused") == 0);

    /* An unrecognised GUID returns a non-NULL string. */
    static const uint8_t unknown[16] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE,
        0xBA, 0xBE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    KTEST_ASSERT(part_guid_type_name(unknown) != 0);

    ktest_summary();
}



/* ---------------------------------------------------------------------------
 * Suite: PMM
 *
 * Exercises pmm_alloc_frame / pmm_free_frame using the live allocator.
 * All allocated frames are freed before the suite returns so the PMM state
 * is identical before and after.
 * ------------------------------------------------------------------------- */

static void test_pmm(void)
{
    ktest_begin("pmm");

    /* Alloc must return a 4 KiB-aligned non-error address. */
    uint32_t f1 = pmm_alloc_frame();
    KTEST_ASSERT(f1 != PMM_ALLOC_ERROR);
    KTEST_ASSERT((f1 & 0xFFFu) == 0);

    /* Two consecutive allocs must return distinct frames. */
    uint32_t f2 = pmm_alloc_frame();
    KTEST_ASSERT(f2 != PMM_ALLOC_ERROR);
    KTEST_ASSERT(f1 != f2);

    /* free_count decreases by 1 per alloc. */
    uint32_t fc = pmm_free_count();
    uint32_t f3 = pmm_alloc_frame();
    KTEST_ASSERT(f3 != PMM_ALLOC_ERROR);
    KTEST_ASSERT(pmm_free_count() == fc - 1);

    /* Freeing a frame increments free_count and makes it re-allocatable. */
    pmm_free_frame(f3);
    KTEST_ASSERT(pmm_free_count() == fc);
    uint32_t f4 = pmm_alloc_frame();
    KTEST_ASSERT(f4 == f3);   /* same frame recycled (first-fit scan) */

    pmm_free_frame(f1);
    pmm_free_frame(f2);
    pmm_free_frame(f4);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: heap
 *
 * Tests the kmalloc/kfree/krealloc first-fit allocator.
 * ------------------------------------------------------------------------- */

static void test_heap(void)
{
    ktest_begin("heap");

    /* kmalloc(0) must return NULL. */
    KTEST_ASSERT(kmalloc(0) == NULL);

    /* Normal allocations return non-NULL distinct pointers. */
    void *p1 = kmalloc(64);
    void *p2 = kmalloc(64);
    KTEST_ASSERT(p1 != NULL);
    KTEST_ASSERT(p2 != NULL);
    KTEST_ASSERT(p1 != p2);

    /* Data written to one block is not clobbered by the other. */
    memset(p1, 0xAB, 64);
    memset(p2, 0xCD, 64);
    KTEST_ASSERT(((uint8_t *)p1)[0]  == 0xAB);
    KTEST_ASSERT(((uint8_t *)p1)[63] == 0xAB);
    KTEST_ASSERT(((uint8_t *)p2)[0]  == 0xCD);

    /* kfree(NULL) must not crash. */
    kfree(NULL);

    /* krealloc to a larger size preserves existing bytes. */
    void *p3 = kmalloc(16);
    KTEST_ASSERT(p3 != NULL);
    memset(p3, 0xEF, 16);
    void *p4 = krealloc(p3, 128);
    KTEST_ASSERT(p4 != NULL);
    KTEST_ASSERT(((uint8_t *)p4)[0]  == 0xEF);
    KTEST_ASSERT(((uint8_t *)p4)[15] == 0xEF);

    /* krealloc(ptr, 0) behaves like kfree and returns NULL. */
    void *p5 = kmalloc(32);
    KTEST_ASSERT(p5 != NULL);
    void *p6 = krealloc(p5, 0);
    KTEST_ASSERT(p6 == NULL);

    /* After freeing, the allocator can hand out a new block in that space. */
    kfree(p1);
    void *p7 = kmalloc(64);
    KTEST_ASSERT(p7 != NULL);

    kfree(p2);
    kfree(p4);
    kfree(p7);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: VMM
 *
 * Tests vmm_create_pd / vmm_map_page / vmm_unmap_page / vmm_free_pd.
 *
 * Page table entries are inspected directly: since the kernel is identity-
 * mapped (phys == virt), every PMM frame address is directly dereferenceable
 * as a uint32_t pointer.
 *
 * x86 page-entry bit positions used here:
 *   bit 0 (0x1)  – Present
 *   bit 7 (0x80) – PS / large page (set in kernel PSE entries)
 * ------------------------------------------------------------------------- */

#define KT_PAGE_PRESENT  0x1u
#define KT_PAGE_LARGE    0x80u

static void test_vmm(void)
{
    ktest_begin("vmm");

    uint32_t *kpd = paging_kernel_pd();

    /* vmm_create_pd returns a 4 KiB-aligned pointer. */
    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);
    KTEST_ASSERT(((uint32_t)pd & 0xFFFu) == 0);

    /* Kernel PDEs (indices 0–63) are propagated into the new PD. */
    KTEST_ASSERT(pd[0]  == kpd[0]);
    KTEST_ASSERT(pd[32] == kpd[32]);
    KTEST_ASSERT(pd[63] == kpd[63]);

    /* Non-kernel PDEs are zeroed. */
    KTEST_ASSERT((pd[64]   & KT_PAGE_PRESENT) == 0);
    KTEST_ASSERT((pd[1023] & KT_PAGE_PRESENT) == 0);

    /* vmm_map_page: allocate a frame and map it at a user virtual address. */
    uint32_t phys = pmm_alloc_frame();
    KTEST_ASSERT(phys != PMM_ALLOC_ERROR);

    uint32_t virt = 0x40001000u;               /* PDE 256, PTE 1 */
    uint32_t pdi  = virt >> 22;                /* 256             */
    uint32_t pti  = (virt >> 12) & 0x3FFu;    /* 1               */

    vmm_map_page(pd, virt, phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /* PDE for the mapped region must be present and NOT a large page. */
    KTEST_ASSERT((pd[pdi] & KT_PAGE_PRESENT) != 0);
    KTEST_ASSERT((pd[pdi] & KT_PAGE_LARGE)   == 0);

    /* The PTE must point to the right physical frame with Present set. */
    uint32_t *pt = (uint32_t *)(pd[pdi] & ~0xFFFu);
    KTEST_ASSERT((pt[pti] & KT_PAGE_PRESENT) != 0);
    KTEST_ASSERT((pt[pti] & ~0xFFFu) == phys);

    /* Mapping inside the kernel large-page window is silently ignored. */
    vmm_map_page(pd, 0x1000u, phys, VMM_FLAG_USER);  /* PDE 0 = large page */
    KTEST_ASSERT(pd[0] == kpd[0]);                    /* PDE 0 unchanged    */

    /* vmm_unmap_page clears the PTE (does not free the frame). */
    vmm_unmap_page(pd, virt);
    KTEST_ASSERT((pt[pti] & KT_PAGE_PRESENT) == 0);

    /* Re-map so vmm_free_pd has a frame to release. */
    vmm_map_page(pd, virt, phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /* vmm_free_pd releases the mapped frame, the page table, and the PD itself.
     * Three PMM frames total must be returned. */
    uint32_t fc_before = pmm_free_count();
    vmm_free_pd(pd);                           /* frees phys + PT + PD = 3 */
    KTEST_ASSERT(pmm_free_count() == fc_before + 3);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: task
 *
 * Exercises task_create and task_yield using the live task pool.
 * Two lightweight noop tasks are created; task_yield transfers control to them
 * and they self-terminate via task_exit, eventually returning here.
 * ------------------------------------------------------------------------- */

static void noop_task(void) { task_exit(); }

static void test_task(void)
{
    ktest_begin("task");

    /* task_create must return a non-NULL pointer. */
    task_t *t1 = task_create("ktest_noop1", noop_task);
    KTEST_ASSERT(t1 != NULL);

    /* A second create must return a different (distinct) task slot. */
    task_t *t2 = task_create("ktest_noop2", noop_task);
    KTEST_ASSERT(t2 != NULL);
    KTEST_ASSERT(t1 != t2);

    /* task_yield must not crash; the noop tasks run and exit, then control
     * returns here via the cooperative scheduler. */
    task_yield();
    KTEST_ASSERT(1);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: syscall
 *
 * Calls syscall_dispatch directly with a stack-allocated registers_t frame,
 * verifying that safe syscalls do not crash and return control to the caller.
 * SYS_EXIT is intentionally excluded — it calls task_exit() which is noreturn.
 * ------------------------------------------------------------------------- */

static void test_syscall(void)
{
    ktest_begin("syscall");

    registers_t regs;
    memset(&regs, 0, sizeof(regs));

    /* SYS_WRITE with a valid NUL-terminated kernel string must not crash. */
    regs.eax = SYS_WRITE;
    regs.ebx = (uint32_t)"[ktest] syscall SYS_WRITE\n";
    syscall_dispatch(&regs);
    KTEST_ASSERT(1);

    /* SYS_YIELD must not crash (internally calls task_yield). */
    regs.eax = SYS_YIELD;
    regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(1);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: GDT
 *
 * Verifies that the GDT segment descriptors ring-3 entry depends on are
 * installed correctly:
 *   index 3 (selector 0x1B) — user code,  DPL=3
 *   index 4 (selector 0x23) — user data,  DPL=3
 *   index 5 (selector 0x28) — TSS,        type=9 (32-bit available)
 *
 * Also round-trips tss_set_kernel_stack / tss_get_esp0 to confirm the TSS
 * ESP0 field is writable (the CPU reads it on every ring-3 → ring-0 entry).
 * ------------------------------------------------------------------------- */

static void test_gdt(void)
{
    extern gdt_entry_t gdt_entries[6];

    ktest_begin("gdt");

    /* User code segment (index 3): DPL field (bits [6:5] of access) must be 3. */
    KTEST_ASSERT(((gdt_entries[3].access >> 5) & 0x3u) == 3u);

    /* User data segment (index 4): DPL must be 3. */
    KTEST_ASSERT(((gdt_entries[4].access >> 5) & 0x3u) == 3u);

    /* TSS descriptor (index 5): low nibble of access byte encodes type.
     * 0x89 → type nibble = 9 = "32-bit TSS available". */
    KTEST_ASSERT((gdt_entries[5].access & 0x0Fu) == 9u);

    /* tss_set_kernel_stack must update the TSS ESP0 field the CPU will read. */
    uint32_t saved = tss_get_esp0();
    tss_set_kernel_stack(0xDEAD0000u);
    KTEST_ASSERT(tss_get_esp0() == 0xDEAD0000u);
    tss_set_kernel_stack(saved);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_prereqs
 *
 * Maps a code page (USER only, not writable) and a stack page (USER+WRITABLE)
 * into a fresh page directory and verifies that the PTE flag bits match what
 * ring3_enter and the CPU require:
 *
 *   Code  page: bit 2 (USER) set, bit 1 (WRITABLE) clear, bit 0 (PRESENT) set
 *   Stack page: bits 2+1+0 all set
 *
 * Uses the same virtual addresses as usertest.c so a mis-mapping here would
 * reproduce the ring-3 freeze without actually entering ring 3.
 * ------------------------------------------------------------------------- */

#define RT_USER_CODE_BASE  0x40000000u   /* PDE 256, PTE 0 */
#define RT_USER_STACK_VIRT 0xBFFEF000u   /* USER_STACK_TOP − 4 KiB */

static void test_ring3_prereqs(void)
{
    ktest_begin("ring3_prereqs");

    /* VMM flag constants must match the x86 PTE bit positions the CPU checks. */
    KTEST_ASSERT(VMM_FLAG_USER     == 0x4u);
    KTEST_ASSERT(VMM_FLAG_WRITABLE == 0x2u);

    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);

    uint32_t phys_code  = pmm_alloc_frame();
    uint32_t phys_stack = pmm_alloc_frame();
    KTEST_ASSERT(phys_code  != PMM_ALLOC_ERROR);
    KTEST_ASSERT(phys_stack != PMM_ALLOC_ERROR);

    /* Code page — user-readable, not writable (ring 3 must not write .text). */
    vmm_map_page(pd, RT_USER_CODE_BASE, phys_code, VMM_FLAG_USER);
    uint32_t pdi_code = RT_USER_CODE_BASE >> 22;
    uint32_t *pt_code = (uint32_t *)(pd[pdi_code] & ~0xFFFu);
    uint32_t pte_code = pt_code[0];
    KTEST_ASSERT((pte_code & 0x1u) != 0);   /* PRESENT  */
    KTEST_ASSERT((pte_code & 0x4u) != 0);   /* USER     */
    KTEST_ASSERT((pte_code & 0x2u) == 0);   /* !WRITABLE */

    /* Stack page — user-readable and writable. */
    vmm_map_page(pd, RT_USER_STACK_VIRT, phys_stack,
                 VMM_FLAG_USER | VMM_FLAG_WRITABLE);
    uint32_t pdi_stack = RT_USER_STACK_VIRT >> 22;
    uint32_t pti_stack = (RT_USER_STACK_VIRT >> 12) & 0x3FFu;
    uint32_t *pt_stack = (uint32_t *)(pd[pdi_stack] & ~0xFFFu);
    uint32_t pte_stack = pt_stack[pti_stack];
    KTEST_ASSERT((pte_stack & 0x1u) != 0);  /* PRESENT   */
    KTEST_ASSERT((pte_stack & 0x4u) != 0);  /* USER      */
    KTEST_ASSERT((pte_stack & 0x2u) != 0);  /* WRITABLE  */

    /* vmm_free_pd releases the mapped frames, page tables, and the PD itself. */
    uint32_t fc_before = pmm_free_count();
    vmm_free_pd(pd);    /* frees phys_code + PT_code + phys_stack + PT_stack + PD = 5 */
    KTEST_ASSERT(pmm_free_count() == fc_before + 5);

    ktest_summary();
}

int ktest_run_all(void)
{
    int total_pass = 0;
    int total_fail = 0;

    test_acpi_checksum();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_string();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_partition();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_pmm();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_heap();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vmm();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_task();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_syscall();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_gdt();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_prereqs();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    t_writestring("\n[ktest] TOTAL: ");
    t_dec((uint32_t)total_pass);
    t_writestring(" passed, ");
    t_dec((uint32_t)total_fail);
    t_writestring(" failed\n");
    return total_fail;
}
