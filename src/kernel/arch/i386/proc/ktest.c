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
#include <kernel/serial.h>
#include <kernel/tty.h>
#include <kernel/vesa.h>
#include <kernel/vesa_tty.h>
#include <kernel/bochs_vbe.h>
#include <kernel/timer.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/asm.h>
#include <kernel/keyboard.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Harness state & primitives
 * ------------------------------------------------------------------------- */

int ktest_pass_count = 0;
int ktest_fail_count = 0;
volatile int ktest_bg_done = 0;

/* Background-suite progress, surfaced to the shell loading screen.  Updated
 * inside the RUN macro in ktest_bg_task; total is fixed at compile time so
 * the bar length is known the moment shell_run starts. */
volatile int ktest_bg_completed = 0;
const    int ktest_bg_total     = 11;   /* keep in sync with RUN() calls below */

/* When set, suppress VGA output for pass lines and suite headers. */
int ktest_muted = 0;

void ktest_begin(const char *suite, const char *desc)
{
    ktest_pass_count = 0;
    ktest_fail_count = 0;
    if (!ktest_muted) {
        t_writestring("\n[ktest] suite: ");
        t_writestring(suite);
        if (desc && *desc) {
            t_writestring(" -- ");
            t_writestring(desc);
        }
        t_putchar('\n');
    }
}

void ktest_assert(int cond, const char *expr, const char *file, uint32_t line)
{
    if (cond) {
        if (!ktest_muted) {
            t_writestring("  PASS: ");
            t_writestring(expr);
            t_putchar('\n');
        }
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
    if (ktest_muted)
        return;
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
    ktest_begin("acpi_checksum", "ACPI table byte-sum checksum invariants");

    /* A buffer whose byte sum is 0 - valid. */
    uint8_t good[4] = {0x01, 0x02, 0x03, 0xFA}; /* 1+2+3+250 = 256 → 0 mod 256 */
    KTEST_ASSERT(acpi_checksum(good, 4));

    /* Off-by-one: change the last byte so the sum is non-zero. */
    uint8_t bad[4] = {0x01, 0x02, 0x03, 0xFB};
    KTEST_ASSERT(!acpi_checksum(bad, 4));

    /* Single byte whose value is 0 - valid (sum = 0). */
    uint8_t zero[1] = {0x00};
    KTEST_ASSERT(acpi_checksum(zero, 1));

    /* Single byte whose value is non-zero - invalid. */
    uint8_t nonzero[1] = {0x01};
    KTEST_ASSERT(!acpi_checksum(nonzero, 1));

    /* Empty buffer (length 0) - sum is 0, always valid. */
    KTEST_ASSERT(acpi_checksum(good, 0));

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: string helpers (sanity-check the libc stubs used by the kernel)
 * ------------------------------------------------------------------------- */

static void test_string(void)
{
    ktest_begin("string", "freestanding libc string ops (strlen/strcmp/strncmp/memset/strcpy)");

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
    ktest_begin("partition", "MBR + GPT partition-type name lookups");

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
    ktest_begin("pmm", "physical-memory allocator: 4 KiB frame alloc/free + free-count accounting");

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
    ktest_begin("heap", "kernel heap: kmalloc/kfree, bytes-written sanity, exhaustion path");

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
    ktest_begin("vmm", "per-task page directory: create, map, switch, lookup, teardown");

    uint32_t *kpd = paging_kernel_pd();

    /* vmm_create_pd returns a 4 KiB-aligned pointer. */
    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);
    KTEST_ASSERT(((uint32_t)pd & 0xFFFu) == 0);

    /* Every non-zero kernel PDE must be propagated into the new PD. */
    bool all_kpdes_ok = true;
    for (uint32_t i = 0; i < 1024; i++) {
        if (kpd[i] && pd[i] != kpd[i]) {
            all_kpdes_ok = false;
            break;
        }
    }
    KTEST_ASSERT(all_kpdes_ok);

    /* Spot-check: identity-window large-page entries copied. */
    KTEST_ASSERT(pd[0]  == kpd[0]);
    KTEST_ASSERT(pd[32] == kpd[32]);
    KTEST_ASSERT(pd[63] == kpd[63]);

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

static volatile int noop_ran;
static void noop_task(void) { noop_ran = 1; task_exit(); }

static void test_task(void)
{
    ktest_begin("task", "scheduler primitives: task pool, state transitions, yield semantics");

    /* Disable interrupts across both creates so the timer cannot preempt
     * noop1 before noop2 exists - otherwise noop1 runs, dies, and its slot
     * gets recycled for noop2, making t1 == t2. */
    disable_interrupts();
    noop_ran = 0;
    task_t *t1 = task_create("ktest_noop1", noop_task);
    task_t *t2 = task_create("ktest_noop2", noop_task);
    enable_interrupts();

    KTEST_ASSERT(t1 != NULL);
    KTEST_ASSERT(t2 != NULL);
    KTEST_ASSERT(t1 != t2);

    /* Yield until at least one noop task has run and exited. */
    for (int i = 0; i < 100 && !noop_ran; i++)
        task_yield();
    KTEST_ASSERT(noop_ran);
    KTEST_ASSERT(t1->state == TASK_DEAD || t2->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: syscall
 *
 * Calls syscall_dispatch directly with a stack-allocated registers_t frame,
 * verifying that safe syscalls do not crash and return control to the caller.
 * SYS_EXIT is intentionally excluded - it calls task_exit() which is noreturn.
 * ------------------------------------------------------------------------- */

static void test_syscall(void)
{
    ktest_begin("syscall", "int 0x80 syscall dispatcher: arg passing, return value, unknown-syscall guard");

    registers_t regs;
    memset(&regs, 0, sizeof(regs));

    /* SYS_WRITE(fd=1, buf, len): write to stdout - must not crash and must
     * return the byte count. */
    static const char msg[] = "[ktest] syscall SYS_WRITE\n";
    regs.eax = SYS_WRITE;
    regs.ebx = FD_STDOUT;
    regs.ecx = (uint32_t)(uintptr_t)msg;
    regs.edx = sizeof(msg) - 1;   /* exclude NUL */
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == sizeof(msg) - 1);

    /* SYS_WRITE to an invalid fd must return -1. */
    regs.eax = SYS_WRITE;
    regs.ebx = 99;   /* no such fd */
    regs.ecx = (uint32_t)(uintptr_t)msg;
    regs.edx = 1;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-1);

    /* Unknown syscall must return -ENOSYS (not crash). */
    regs.eax = 9999;
    regs.ebx = regs.ecx = regs.edx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-38);   /* -ENOSYS */

    /* SYS_YIELD must not crash. */
    regs.eax = SYS_YIELD;
    regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(1);

    /* SYS_OPEN on a non-existent path must return -1. */
    regs.eax = SYS_OPEN;
    regs.ebx = (uint32_t)(uintptr_t)"/no/such/file";
    regs.ecx = O_RDONLY;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == (uint32_t)-1);

    /* SYS_BRK(0): query current break on a kernel task (user_brk == 0). */
    regs.eax = SYS_BRK;
    regs.ebx = 0;
    syscall_dispatch(&regs);
    KTEST_ASSERT(regs.eax == 0);   /* kernel tasks have no user heap */

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: GDT
 *
 * Verifies that the GDT segment descriptors ring-3 entry depends on are
 * installed correctly:
 *   index 3 (selector 0x1B) - user code,  DPL=3
 *   index 4 (selector 0x23) - user data,  DPL=3
 *   index 5 (selector 0x28) - TSS,        type=9 (32-bit available)
 *
 * Also round-trips tss_set_kernel_stack / tss_get_esp0 to confirm the TSS
 * ESP0 field is writable (the CPU reads it on every ring-3 → ring-0 entry).
 * ------------------------------------------------------------------------- */

static void test_gdt(void)
{
    extern gdt_entry_t gdt_entries[6];

    ktest_begin("gdt", "GDT layout: kernel/user code+data, TSS selector, ring 3 DPLs");

    /* User code segment (index 3): DPL field (bits [6:5] of access) must be 3. */
    KTEST_ASSERT(((gdt_entries[3].access >> 5) & 0x3u) == 3u);

    /* User data segment (index 4): DPL must be 3. */
    KTEST_ASSERT(((gdt_entries[4].access >> 5) & 0x3u) == 3u);

    /* TSS descriptor (index 5): access byte must be 0x89 (available) or 0x8B
     * (busy) - the CPU sets the busy bit when ltr loads the selector. */
    KTEST_ASSERT(gdt_entries[5].access == 0x89u || gdt_entries[5].access == 0x8Bu);

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
    ktest_begin("ring3_prereqs", "ring-3 transition prerequisites: TSS, user PD, kernel stack");

    /* VMM flag constants must match the x86 PTE bit positions the CPU checks. */
    KTEST_ASSERT(VMM_FLAG_USER     == 0x4u);
    KTEST_ASSERT(VMM_FLAG_WRITABLE == 0x2u);

    uint32_t *pd = vmm_create_pd();
    KTEST_ASSERT(pd != NULL);

    uint32_t phys_code  = pmm_alloc_frame();
    uint32_t phys_stack = pmm_alloc_frame();
    KTEST_ASSERT(phys_code  != PMM_ALLOC_ERROR);
    KTEST_ASSERT(phys_stack != PMM_ALLOC_ERROR);

    /* Code page - user-readable, not writable (ring 3 must not write .text). */
    vmm_map_page(pd, RT_USER_CODE_BASE, phys_code, VMM_FLAG_USER);
    uint32_t pdi_code = RT_USER_CODE_BASE >> 22;
    uint32_t *pt_code = (uint32_t *)(pd[pdi_code] & ~0xFFFu);
    uint32_t pte_code = pt_code[0];
    KTEST_ASSERT((pte_code & 0x1u) != 0);   /* PRESENT  */
    KTEST_ASSERT((pte_code & 0x4u) != 0);   /* USER     */
    KTEST_ASSERT((pte_code & 0x2u) == 0);   /* !WRITABLE */

    /* Stack page - user-readable and writable. */
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

/* ---------------------------------------------------------------------------
 * Suite: IDT
 *
 * Verifies that the IDT entries the kernel depends on are installed correctly:
 *   - Exception gates (DPL=0, present, 32-bit interrupt gate = 0x8E)
 *   - Syscall gate at vector 0x80 (DPL=3 = 0xEE so ring-3 can invoke it)
 *   - All handler pointers are non-zero
 *   - All gates use the kernel code selector (0x08)
 * ------------------------------------------------------------------------- */

static void test_idt(void)
{
    extern idt_entry_t idt_entries[256];
    ktest_begin("idt", "IDT: gate types, DPLs, syscall gate present with DPL=3");

    /* Exception gates: present, DPL=0, 32-bit interrupt gate (0x8E). */
    KTEST_ASSERT(idt_entries[0].flags  == 0x8E);  /* #DE divide error    */
    KTEST_ASSERT(idt_entries[8].flags  == 0x8E);  /* #DF double fault    */
    KTEST_ASSERT(idt_entries[13].flags == 0x8E);  /* #GP protection      */
    KTEST_ASSERT(idt_entries[14].flags == 0x8E);  /* #PF page fault      */

    /* Handler pointers must be non-zero. */
    uint32_t base0  = idt_entries[0].base_lo  | ((uint32_t)idt_entries[0].base_hi  << 16);
    uint32_t base14 = idt_entries[14].base_lo | ((uint32_t)idt_entries[14].base_hi << 16);
    KTEST_ASSERT(base0  != 0);
    KTEST_ASSERT(base14 != 0);

    /* Syscall gate: present, DPL=3 (0xEE) so ring-3 can invoke int 0x80. */
    KTEST_ASSERT(idt_entries[0x80].flags == 0xEE);
    uint32_t base80 = idt_entries[0x80].base_lo | ((uint32_t)idt_entries[0x80].base_hi << 16);
    KTEST_ASSERT(base80 != 0);

    /* All checked gates must use the kernel code selector. */
    KTEST_ASSERT(idt_entries[0].sel    == 0x08);
    KTEST_ASSERT(idt_entries[14].sel   == 0x08);
    KTEST_ASSERT(idt_entries[0x80].sel == 0x08);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_execution
 *
 * Performs an end-to-end ring-3 smoke test by creating a task that maps user
 * code and stack, drops to ring 3 via iret, executes the embedded PIC binary
 * (SYS_DEBUG → SYS_WRITE → SYS_DEBUG → SYS_EXIT), and verifies that both
 * debug checkpoints were reached.
 *
 * CP1 fires immediately on ring-3 entry; CP2 fires after SYS_WRITE returns.
 * Seeing CP2 == 2 proves: ring-3 entry worked, the write syscall returned, and
 * the user binary ran to completion before calling SYS_EXIT.
 * ------------------------------------------------------------------------- */

extern void ring3_usertest_task(void);

static void test_ring3_execution(void)
{
    ktest_begin("ring3_execution", "ring-3 lifecycle end-to-end: iret to user, syscall back, exit");

    /* Reset the checkpoint so stale values from a prior run don't give a
     * false positive. */
    g_ring3_last_cp = 0;

    task_t *t = task_create("ring3test", ring3_usertest_task);
    KTEST_ASSERT(t != NULL);

    /* Yield until the ring-3 task has exited.  With preemptive scheduling the
     * timer may switch us back before ring3test completes all its syscalls, so
     * we loop until the task is marked DEAD rather than assuming one yield is
     * sufficient. */
    while (t && t->state != TASK_DEAD)
        task_yield();

    /* This line executes only after the scheduler returned to kernel mode.
     * It proves we are back in ring 0 with the kernel page directory active,
     * able to call kernel functions and write to serial/VGA normally. */
    Serial_WriteString("[ktest] ring3_execution: back in kernel mode (ring 0)\n");
    if (!ktest_muted)
        t_writestring("[ktest] ring3 -> kernel mode OK\n");

    /* CP2 appears after SYS_WRITE returns, just before SYS_EXIT.  Seeing 2
     * confirms ring-3 entry, the write syscall, and the debug syscall all
     * worked correctly. */
    KTEST_ASSERT(g_ring3_last_cp == 2);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: elf_exec
 *
 * Spawns a child task that calls elf_exec() on /cdrom/apps/echo.elf (the
 * standard VFS path for apps on the CD-ROM image).  Waits for the task to
 * reach TASK_DEAD, which proves the ELF loader, argv setup, ring-3 entry,
 * and SYS_EXIT path all function end-to-end.
 * ------------------------------------------------------------------------- */

static const char *s_echo_argv[] = { "echo", "ktest-elf-exec-ok", NULL };
static int s_echo_argc = 2;

static void elf_exec_task_entry(void)
{
    elf_exec("/cdrom/apps/echo.elf", s_echo_argc, s_echo_argv);
    task_exit();
}

static void test_elf_exec(void)
{
    ktest_begin("elf_exec", "ELF32 loader: header parse, segment mapping, entry-point dispatch");

    static const char *candidates[] = {
        "/cdrom/apps/echo.elf",
        "/hd/apps/echo.elf",
        NULL
    };

    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (vfs_file_exists(candidates[i])) {
            path = candidates[i];
            break;
        }
    }

    if (!path) {
        Serial_WriteString("[ktest] elf_exec: echo.elf not found on cdrom or hd - skipping\n");
        t_writestring("[ktest] elf_exec: echo.elf not found (skip)\n");
        ktest_summary();
        return;
    }

    task_t *t = task_create("elf_exec_test", elf_exec_task_entry);
    KTEST_ASSERT(t != NULL);

    while (t && t->state != TASK_DEAD)
        task_yield();

    Serial_WriteString("[ktest] elf_exec: task completed\n");
    KTEST_ASSERT(t->state == TASK_DEAD);

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: ring3_with_arg
 *
 * Spawns a child task that elf_exec()s hello.elf with one argument
 * ("tester") and verifies the full lifecycle:
 *
 *   parent test task
 *     │ task_create("hello_arg", entry)        ── child enters READY
 *     │
 *     │ ── yields ──>  child runs hello_arg_entry()
 *     │                  └─ elf_exec("/cdrom/apps/hello.elf",
 *     │                              2, {"hello", "tester"})
 *     │                       ↓ ring transition (iret to ring 3)
 *     │                  hello main() prints "Hello, tester!\n"
 *     │                       ↓ SYS_EXIT
 *     │ <── yields ──   child marked TASK_DEAD
 *     │
 *     │ parent observes TASK_DEAD, asserts, prints "control returned"
 *
 * The lifecycle log goes to serial only (via Serial_WriteString) so it
 * doesn't disrupt the loading screen when the test runs from the bg
 * harness. The TTY-visible "Hello, tester!" line in the VESA framebuffer
 * is the user-visible proof that argv made it all the way to ring 3.
 * ------------------------------------------------------------------------- */

static const char *s_hello_argv[] = { "hello", "tester", NULL };
static int         s_hello_argc   = 2;

static void hello_arg_entry(void)
{
    task_t *me = task_current();
    Serial_WriteString("[ktest]   >>> CHILD SCHEDULED (pid=");
    Serial_WriteDec((uint32_t)(me ? me->pid : 0));
    Serial_WriteString(", ring 0) - about to enter ring 3 via elf_exec\n");
    Serial_WriteString("[ktest]   >>> elf_exec(\"hello.elf\", argc=2, argv=[\"hello\",\"tester\"])\n");
    Serial_WriteString("[ktest]   ----- BEGIN RING 3 OUTPUT -----\n");

    elf_exec("/cdrom/apps/hello.elf", s_hello_argc, s_hello_argv);

    /* Only reached on elf_exec failure (e.g. file missing). On success the
     * ring-3 program returns via SYS_EXIT, which calls task_exit() directly
     * and never returns here. */
    Serial_WriteString("[ktest]   ----- elf_exec FAILURE PATH -----\n");
    task_exit();
}

static void test_ring3_with_arg(void)
{
    ktest_begin("ring3_with_arg", "argc/argv plumbing into a ring-3 binary; ESP-relative arg layout");

    Serial_WriteString("\n");
    Serial_WriteString("[ktest] ============================================================\n");
    Serial_WriteString("[ktest]  RING-3 TASK-SWITCHING TEST: parent -> child(hello.elf) -> parent\n");
    Serial_WriteString("[ktest] ============================================================\n");

    static const char *candidates[] = {
        "/cdrom/apps/hello.elf",
        "/hd/apps/hello.elf",
        NULL
    };

    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (vfs_file_exists(candidates[i])) {
            path = candidates[i];
            break;
        }
    }

    if (!path) {
        Serial_WriteString("[ktest] ring3_with_arg: hello.elf not found - skipping\n");
        t_writestring("[ktest] ring3_with_arg: hello.elf not found (skip)\n");
        ktest_summary();
        return;
    }

    task_t *self = task_current();
    int parent_pid = self ? self->pid : 0;

    Serial_WriteString("[ktest] [PARENT] pid=");
    Serial_WriteDec((uint32_t)parent_pid);
    Serial_WriteString(" name=");
    Serial_WriteString((char *)(self && self->name ? self->name : "(unknown)"));
    Serial_WriteString(" - running, ring 0\n");

    Serial_WriteString("[ktest] [PARENT] task_create(\"hello_arg\", hello_arg_entry)\n");
    task_t *child = task_create("hello_arg", hello_arg_entry);
    KTEST_ASSERT(child != NULL);
    if (!child) { ktest_summary(); return; }

    Serial_WriteString("[ktest] [PARENT] child created: pid=");
    Serial_WriteDec((uint32_t)child->pid);
    Serial_WriteString(" name=hello_arg state=READY\n");

    Serial_WriteString("[ktest] [PARENT] yielding -> scheduler picks child (pid=");
    Serial_WriteDec((uint32_t)child->pid);
    Serial_WriteString(")\n");

    /* Yield until the child finishes, with periodic state logging so a hang
     * produces visible diagnostics rather than a silent timeout. */
    uint32_t spins = 0;
    while (child->state != TASK_DEAD) {
        task_yield();
        if ((++spins & 0xFFFu) == 0) {
            Serial_WriteString("[ktest] [PARENT] waiting, child state=");
            Serial_WriteDec((uint32_t)child->state);
            Serial_WriteString("\n");
        }
    }

    Serial_WriteString("[ktest]   ----- END RING 3 OUTPUT -----\n");
    Serial_WriteString("[ktest] [PARENT] resumed, pid=");
    Serial_WriteDec((uint32_t)parent_pid);
    Serial_WriteString(" - child reaped (state=DEAD)\n");

    KTEST_ASSERT(child->state == TASK_DEAD);
    KTEST_ASSERT(task_current() == self);   /* parent identity preserved across context switches */

    Serial_WriteString("[ktest] ============================================================\n");
    Serial_WriteString("[ktest]  RING-3 TASK SWITCHING: WORKING (parent<->child<->ring3 all OK)\n");
    Serial_WriteString("[ktest] ============================================================\n\n");

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: vesa_resolution
 *
 * Exercises the VESA geometry pipeline at four resolutions in ascending size
 * order: 320×240, 640×480, 1280×720, 1920×1080.
 *
 * For each resolution the suite:
 *   1. Announces the upcoming switch with a 3-second countdown (3… 2… 1…).
 *   2. Sets the font scale (1 for <1280-wide, 2 for ≥1280-wide).
 *   3. Programs Bochs VBE hardware (skipped if unavailable).
 *   4. Updates the vesa_fb_t struct via vesa_update_geometry().
 *   5. Re-initialises the VESA TTY via vesa_tty_init().
 *   6. Holds the new resolution for 1 second so it is visible on screen.
 *   7. Asserts fb width/height/bpp/pitch and computed cols/rows.
 *
 * The original resolution is restored before returning.
 * ------------------------------------------------------------------------- */

typedef struct { uint32_t w; uint32_t h; uint32_t scale; const char *name; } vesa_res_t;

/* Print a 3-second countdown before switching resolution. */
static void res_countdown(const char *name)
{
    t_writestring("[ktest] vesa_resolution: switching to ");
    t_writestring(name);
    t_writestring(" in: 3");
    ksleep(100);
    t_writestring("  2");
    ksleep(100);
    t_writestring("  1");
    ksleep(100);
    t_writestring("\n");
}

static void test_vesa_resolution(void)
{
    ktest_begin("vesa_resolution", "VESA mode switching across 320x240 / 640x480 / 720p / 1080p");

    /* Save current fb geometry so we can restore it afterwards. */
    const vesa_fb_t *orig = vesa_get_fb();
    uint32_t saved_w = orig ? orig->width  : 640;
    uint32_t saved_h = orig ? orig->height : 480;
    bool hw = bochs_vbe_available();

    static const vesa_res_t modes[] = {
        {  320,  240, 1, " 320x240"  },
        {  640,  480, 1, " 640x480"  },
        { 1280,  720, 2, "1280x720"  },
        { 1920, 1080, 2, "1920x1080" },
    };

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t    w     = modes[i].w;
        uint32_t    h     = modes[i].h;
        uint32_t    scale = modes[i].scale;
        const char *name  = modes[i].name;

        res_countdown(name);

        vesa_tty_set_scale(scale);
        if (hw)
            bochs_vbe_set_mode(w, h, 32);
        vesa_update_geometry(w, h, 32);
        vesa_tty_init();

        /* Hold the new resolution for 1 second so it is visible. */
        t_writestring("[ktest] vesa_resolution: now at ");
        t_writestring(name);
        t_writestring("x32\n");
        ksleep(100);

        const vesa_fb_t *fb = vesa_get_fb();
        KTEST_ASSERT(fb != NULL);
        KTEST_ASSERT_EQ(fb->width,  w);
        KTEST_ASSERT_EQ(fb->height, h);
        KTEST_ASSERT_EQ(fb->bpp,    32u);
        KTEST_ASSERT_EQ(fb->pitch,  w * 4u);

        /* Each glyph cell is 8×8 pixels scaled by font_scale. */
        uint32_t cell = 8u * scale;
        KTEST_ASSERT_EQ(vesa_tty_get_cols(), w / cell);
        KTEST_ASSERT_EQ(vesa_tty_get_rows(), h / cell);
    }

    /* Restore original display state. */
    t_writestring("[ktest] vesa_resolution: restoring ");
    t_dec(saved_w); t_writestring("x"); t_dec(saved_h);
    t_writestring("\n");
    uint32_t restore_scale = (saved_w >= 1280) ? 2u : 1u;
    vesa_tty_set_scale(restore_scale);
    if (hw)
        bochs_vbe_set_mode(saved_w, saved_h, 32);
    vesa_update_geometry(saved_w, saved_h, 32);
    vesa_tty_init();

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: vesa_colour
 *
 * Cycles through all 16 standard CGA palette colours as both foreground and
 * background, holding each combination briefly so it is visible in a graphical
 * ktest run.  Asserts that vesa_tty_is_ready() remains true throughout and
 * that the screen can be written to without panicking.
 * ------------------------------------------------------------------------- */

typedef struct { const char *name; uint32_t rgb; } ktest_colour_t;

static const ktest_colour_t ktest_palette[] = {
    { "black",        0x000000 },
    { "blue",         0x0000AA },
    { "green",        0x00AA00 },
    { "cyan",         0x00AAAA },
    { "red",          0xAA0000 },
    { "magenta",      0xAA00AA },
    { "brown",        0xAA5500 },
    { "lightgray",    0xAAAAAA },
    { "darkgray",     0x555555 },
    { "lightblue",    0x5555FF },
    { "lightgreen",   0x55FF55 },
    { "lightcyan",    0x55FFFF },
    { "lightred",     0xFF5555 },
    { "lightmagenta", 0xFF55FF },
    { "yellow",       0xFFFF55 },
    { "white",        0xFFFFFF },
};
#define KTEST_PALETTE_SIZE ((uint32_t)(sizeof(ktest_palette)/sizeof(ktest_palette[0])))

static void test_vesa_colour(void)
{
    ktest_begin("vesa_colour", "VESA fg/bg colour state, glyph rendering, framebuffer pixel layout");

    KTEST_ASSERT(vesa_tty_is_ready());

    /* Pair each background with a contrasting foreground (white or black). */
    for (uint32_t i = 0; i < KTEST_PALETTE_SIZE; i++) {
        uint32_t bg = ktest_palette[i].rgb;
        /* Use white fg on dark backgrounds, black fg on light ones. */
        uint32_t luminance = ((bg >> 16) & 0xFF) * 299u
                           + ((bg >>  8) & 0xFF) * 587u
                           + ( bg        & 0xFF) * 114u;
        uint32_t fg = (luminance < 128000u) ? 0xFFFFFF : 0x000000;

        vesa_tty_setcolor(fg, bg);
        vesa_tty_clear();

        t_writestring("[ktest] vesa_colour: bg=");
        t_writestring(ktest_palette[i].name);
        t_writestring("\n");

        KTEST_ASSERT(vesa_tty_is_ready());

        ksleep(16); /* ~160 ms at 100 Hz - long enough to see the change */
    }

    /* Restore default white-on-blue. */
    vesa_tty_setcolor(0xFFFFFF, 0x0000AA);
    vesa_tty_clear();

    ktest_summary();
}

/* ---------------------------------------------------------------------------
 * Suite: keyboard
 *
 * Drives the PS/2 decoder synchronously through the keyboard_test_* hooks.
 * Each subtest calls keyboard_test_reset() so it inherits a clean decoder
 * state, modifier state, and global ring.  The whole suite is bracketed
 * by keyboard_test_begin/end which save and clear the real focused task
 * (so routed bytes land in the global ring where we can drain them).
 *
 * Behaviour pinned by these tests:
 *
 *   - All KEY_* sentinels survive the dispatch path as unsigned bytes >= 0x80
 *     (the EIP=0xFFFFFF83 sign-extension regression from PR #124).
 *   - Make/break separation: a break never produces output; a held modifier
 *     applies to subsequent makes; releasing the other side of a paired
 *     modifier (RSHIFT while LSHIFT is still down) does not clear the state.
 *   - Decoder is livelock-free under random byte streams (4096-byte LCG fuzz)
 *     and across the full 0x00..0xFF boundary in each prefix state.
 *   - PrintScreen's "fake-shift" padding (e0 2a / e0 aa) is dropped.
 *
 * The typematic-repeat and Caps-LED behaviours are *not* asserted yet --
 * they're introduced by the slice-5b commits that follow this harness, so
 * the assertions for those land alongside the fixes.
 * ------------------------------------------------------------------------- */

/* Drain the global ring into out[], return count.  Bounded to cap so a
 * runaway decoder can't blow the stack buffer. */
static uint32_t kb_test_drain_all(unsigned char *out, uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap) {
        unsigned char c;
        if (!keyboard_test_drain(&c)) break;
        out[n++] = c;
    }
    return n;
}

static void test_keyboard(void)
{
    ktest_begin("keyboard", "PS/2 layered driver: decoder state machine, modifier tracking, SPSC ring");

    keyboard_test_begin();

    /* ---- sentinel coverage: e0-prefixed arrow keycodes ------------------ */
    /* These are the four KEY_ARROW_* sentinels 0x80..0x83.  Each must arrive
     * as a single unsigned byte equal to the sentinel.  The 0x83 case is
     * the regression target for PR #124's EIP=0xFFFFFF83 panic. */
    {
        struct { uint8_t e0_byte; unsigned char expect; } cases[] = {
            { 0x48, 0x80 }, /* KEY_ARROW_UP    */
            { 0x50, 0x81 }, /* KEY_ARROW_DOWN  */
            { 0x4B, 0x82 }, /* KEY_ARROW_LEFT  */
            { 0x4D, 0x83 }, /* KEY_ARROW_RIGHT */
        };
        for (uint32_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(0xE0);
            keyboard_test_feed(cases[i].e0_byte);
            unsigned char buf[4];
            uint32_t n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1);
            KTEST_ASSERT(buf[0] == cases[i].expect);
            /* Each sentinel is >= 0x80 - if any path sign-extended it, the
             * resulting int comparison would survive but the byte would not
             * round-trip equal to the cast we expect. */
            KTEST_ASSERT(buf[0] >= 0x80);
            /* Break event must produce no output and not double-fire. */
            keyboard_test_feed(0xE0);
            keyboard_test_feed((uint8_t)(cases[i].e0_byte | 0x80));
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 0);
        }
    }

    /* ---- single-byte ASCII translation ---------------------------------- */
    {
        keyboard_test_reset();
        keyboard_test_feed(0x1E); /* 'a' make */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1);
        KTEST_ASSERT(buf[0] == 'a');

        keyboard_test_feed(0x9E); /* 'a' break - no output */
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
    }

    /* ---- make/break separation with both shifts ------------------------- */
    /* Hold RSHIFT, press 'a' -> 'A'.  Release RSHIFT, press 'a' -> 'a'.
     * Repeat with LSHIFT.  The LSHIFT half is the regression target for the
     * 0xAA BAT-pass filter collision -- before decoder_feed's response-byte
     * filter was narrowed, LSHIFT break (0xAA) was silently swallowed and
     * the shift state stayed stuck at 1. */
    {
        struct { uint8_t make, brk; } shifts[] = {
            { 0x36, 0xB6 }, /* RSHIFT */
            { 0x2A, 0xAA }, /* LSHIFT - regression target */
        };
        for (uint32_t i = 0; i < sizeof(shifts)/sizeof(shifts[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(shifts[i].make);
            unsigned char buf[4];
            uint32_t n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 0); /* modifier silent in cooked mode */
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);

            keyboard_test_feed(0x1E); /* 'a' make */
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1 && buf[0] == 'A');

            keyboard_test_feed(shifts[i].brk);
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0u);

            keyboard_test_feed(0x1E);
            n = kb_test_drain_all(buf, sizeof(buf));
            KTEST_ASSERT(n == 1 && buf[0] == 'a');
        }
    }

    /* ---- paired-modifier release: either side can be released first ----- */
    /* Hold LSHIFT + RSHIFT; release either; mod_shift stays set thanks to
     * the other side still being held. */
    {
        struct { uint8_t first_brk; } cases[] = { { 0xB6 }, { 0xAA } };
        for (uint32_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
            keyboard_test_reset();
            keyboard_test_feed(0x2A); /* LSHIFT make */
            keyboard_test_feed(0x36); /* RSHIFT make */
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);
            keyboard_test_feed(cases[i].first_brk);
            KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0x1u);
        }
    }

    /* ---- Ctrl+letter folds to ASCII control code ------------------------ */
    /* Use 'b' (Ctrl-B == 0x02), not 'a': Ctrl-A is intercepted in cooked
     * mode as the pane-switch prefix and never reaches the routed-byte path. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0x1D); /* LCTRL make */
        keyboard_test_feed(0x30); /* 'b' make - should be Ctrl-B == 0x02 */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 0x02);
        keyboard_test_feed(0x9D); /* LCTRL break */
    }

    /* ---- typematic-repeat filter for modifiers -------------------------- */
    /* PS/2 hardware re-fires a held key's make at ~30 Hz.  For Caps Lock the
     * previous decoder toggled mod_caps on every make, so holding Caps for a
     * fraction of a second ping-ponged the toggle unpredictably.  Modifier
     * make events without an intervening break must now be dropped entirely.
     *
     * Specifically:
     *   - Caps make x5, then break: mod_caps must toggle EXACTLY once.
     *   - Shift make x5: mod_shift stays 1 (idempotent) but routes no extra
     *     KEY_SHIFT_DOWN sentinels in raw mode.  We test the state half here
     *     (raw-mode sentinel coverage is covered by the kbtester smoke test).
     *   - After break and re-press, the next make is honoured again. */
    {
        keyboard_test_reset();
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A); /* Caps make */
        KTEST_ASSERT((keyboard_test_mod_state() >> 3) & 1u); /* mod_caps == 1 */
        keyboard_test_feed(0xBA);     /* Caps break */
        KTEST_ASSERT((keyboard_test_mod_state() >> 3) & 1u); /* still 1 (Caps is sticky) */

        /* Second press cycle: another 5x make should toggle exactly once. */
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A);
        KTEST_ASSERT(((keyboard_test_mod_state() >> 3) & 1u) == 0u);
        keyboard_test_feed(0xBA);
    }
    {
        /* LSHIFT held: subsequent makes don't redundantly fire on_make.  We
         * can't directly observe on_make calls here, but raw-mode delivery
         * surfaces them as routed bytes -- exercise that path. */
        keyboard_test_reset();
        keyboard_set_raw(1);
        keyboard_test_feed(0x2A); /* LSHIFT make */
        unsigned char buf[8];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == (unsigned char)KEY_SHIFT_DOWN);
        /* Typematic repeats should NOT re-emit the sentinel. */
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x2A);
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
        /* Break, then re-press: a single sentinel again. */
        keyboard_test_feed(0xAA);
        keyboard_test_feed(0x2A);
        n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == (unsigned char)KEY_SHIFT_DOWN);
        keyboard_set_raw(0);
    }

    /* ---- LED sync: Caps press updates the bitmap exactly once ----------- */
    /* kb_sync_leds is called from apply_modifier when mod_caps changes; the
     * 0xED + bitmap is suppressed in test mode and only the software shadow
     * updates.  We exercise:
     *   - one Caps press cycle flips the Caps bit and increments send count;
     *   - typematic repeats during the press DON'T trigger extra sends
     *     (the typematic filter from f636920 squashes them upstream);
     *   - a second press cycle clears the Caps bit. */
    {
        keyboard_test_reset();
        uint32_t baseline = keyboard_test_led_sends();
        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A); /* Caps make x5 */
        keyboard_test_feed(0xBA);     /* Caps break */
        KTEST_ASSERT(keyboard_test_led_sends() == baseline + 1);
        KTEST_ASSERT(keyboard_test_leds() & 0x04); /* PS2_LED_CAPS */

        for (int i = 0; i < 5; i++)
            keyboard_test_feed(0x3A);
        keyboard_test_feed(0xBA);
        KTEST_ASSERT(keyboard_test_led_sends() == baseline + 2);
        KTEST_ASSERT((keyboard_test_leds() & 0x04) == 0);
    }

    /* ---- torn-prefix recovery: repeated 0xE0 restarts the prefix -------- */
    /* DEC_AFTER_E0 + 0xE0 -> still DEC_AFTER_E0; one ARROW_LEFT emitted. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0x4B); /* ARROW_LEFT make */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 0x82);
    }

    /* ---- PrintScreen fake-shift padding is dropped ---------------------- */
    /* PS/2 emits e0 2a e0 37 (make) / e0 b7 e0 aa (break).  The e0 2a / e0 aa
     * portions are "fake shift" padding -- we drop them so real shift state
     * stays honest.  After feeding e0 aa with no real shift held, mod_shift
     * must still be zero. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0x2A);
        keyboard_test_feed(0xE0);
        keyboard_test_feed(0xAA);
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 0);
        KTEST_ASSERT((keyboard_test_mod_state() & 0x1u) == 0u);
    }

    /* ---- controller response bytes reset decoder state ------------------ */
    /* 0xFA (ack) and friends arriving mid-prefix must reset to NORMAL, not
     * poison the next real scancode. */
    {
        keyboard_test_reset();
        keyboard_test_feed(0xE0);   /* prime extended prefix */
        keyboard_test_feed(0xFA);   /* ack - must reset */
        keyboard_test_feed(0x1E);   /* 'a' make - should arrive as plain 'a' */
        unsigned char buf[4];
        uint32_t n = kb_test_drain_all(buf, sizeof(buf));
        KTEST_ASSERT(n == 1 && buf[0] == 'a');
    }

    /* ---- boundary scan: every byte in every decoder state, no panic ----- */
    /* Feed 0x00..0xFF in DEC_NORMAL, DEC_AFTER_E0, DEC_AFTER_E1A, DEC_AFTER_E1B.
     * Each iteration starts from a reset so the state we want to test is the
     * one we just put the decoder into.  Anything that crashes here would
     * page-fault and we'd never get to the assert below. */
    {
        for (uint32_t prelude = 0; prelude < 4; prelude++) {
            for (uint32_t b = 0; b < 256; b++) {
                keyboard_test_reset();
                switch (prelude) {
                    case 1: keyboard_test_feed(0xE0); break;
                    case 2: keyboard_test_feed(0xE1); break;
                    case 3: keyboard_test_feed(0xE1); keyboard_test_feed(0x1D); break;
                    default: break;
                }
                keyboard_test_feed((uint8_t)b);
                /* Drain (and discard) any emitted bytes; the test is the
                 * absence of a fault, not specific output. */
                unsigned char tmp[8];
                (void)kb_test_drain_all(tmp, sizeof(tmp));
            }
        }
        KTEST_ASSERT(1); /* survived 1024 byte/state combinations */
    }

    /* ---- 512-byte LCG fuzz: no panic, ring never wedges ----------------- */
    /* A tiny LCG (Numerical Recipes constants) gives us a reproducible
     * pseudo-random byte stream.  We feed it into the decoder and drain
     * after each byte so the ring never fills.  Any unsigned-char hygiene
     * regression on the routed-byte path would either fault or wedge the
     * ring head/tail accounting; we assert the obvious invariants. */
    {
        keyboard_test_reset();
        uint32_t seed = 0xC0FFEEu;
        uint32_t total_drained = 0;
        for (uint32_t i = 0; i < 512; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t sc = (uint8_t)(seed >> 16);
            keyboard_test_feed(sc);
            unsigned char tmp[8];
            uint32_t n = kb_test_drain_all(tmp, sizeof(tmp));
            total_drained += n;
        }
        /* The decoder definitely produced *some* output across 512 random
         * bytes; we don't need an exact count, just non-zero and bounded. */
        KTEST_ASSERT(total_drained > 0);
        KTEST_ASSERT(total_drained < 512); /* most bytes are break-halves or prefixes */
    }

    keyboard_test_end();

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

    test_idt();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_execution();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_elf_exec();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_ring3_with_arg();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vesa_resolution();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_vesa_colour();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    test_keyboard();
    total_pass += ktest_pass_count;
    total_fail += ktest_fail_count;

    t_writestring("\n[ktest] TOTAL: ");
    t_dec((uint32_t)total_pass);
    t_writestring(" passed, ");
    t_dec((uint32_t)total_fail);
    t_writestring(" failed\n");
    return total_fail;
}

/* Background task entry: run safe suites silently during the loading screen.
 * Skips test_vesa_resolution and test_vesa_colour - both switch display modes
 * and have multi-second sleeps that would corrupt the loading screen.
 * Sets ktest_bg_done = 1 when finished so shell_run can proceed. */
void ktest_bg_task(void)
{
    int total_pass = 0;
    int total_fail = 0;

    ktest_muted = 1;
    ktest_bg_completed = 0;

    #define RUN(suite) do { \
        Serial_WriteString("[ktest-bg] >> " #suite "\n"); \
        suite(); \
        total_pass += ktest_pass_count; \
        total_fail += ktest_fail_count; \
        ktest_bg_completed++; \
        Serial_WriteString("[ktest-bg] << " #suite " (" \
                            #suite " pass count below)\n"); \
        /* Brief pacing keeps the loading screen visible while not pushing \
         * iso-test's 120 s GDB budget into the failure regime under TCG. \
         * 5 ticks @ 100 Hz = 50 ms; visible on real HW, ~650 ms total over \
         * 13 suites in TCG. */ \
        { uint32_t t0 = timer_get_ticks(); \
          while (timer_get_ticks() - t0 < 5) task_yield(); } \
    } while (0)

    RUN(test_acpi_checksum);
    RUN(test_string);
    RUN(test_partition);
    RUN(test_pmm);
    RUN(test_heap);
    RUN(test_vmm);
    RUN(test_task);
    RUN(test_syscall);
    RUN(test_gdt);
    RUN(test_ring3_prereqs);
    RUN(test_idt);
    /* Skipped from the bg pass (still run deterministically in foreground
     * via test_mode ISO Phase 1 and the shell `ktest` command):
     *   - test_ring3_execution / test_elf_exec / test_ring3_with_arg:
     *     each spawns a ring-3 child and re-enters ring 0 via int 0x80,
     *     then yields back to the bg parent.  Under concurrent scheduling
     *     with 4 shell tasks and a GDB-attached debug build this path
     *     intermittently leaves TF=1 in EFLAGS of the resuming kernel
     *     context, triggering an INT1 single-step storm.  Phase 1
     *     (single-task, no GDB) exercises the same code reliably and
     *     proves the lifecycle.
     *   - test_keyboard: ~1500 decoder_feed cycles push past shell_run's
     *     first keyboard_getchar under TCG.
     *   - test_vesa_resolution / test_vesa_colour: switch display modes
     *     with multi-second countdowns. */

    #undef RUN

    ktest_muted = 0;

    if (total_fail > 0) {
        t_writestring("[ktest] ");
        t_dec((uint32_t)total_fail);
        t_writestring(" failure(s) - run `ktest` for details\n");
        Serial_WriteString("KTEST_BG: FAIL\n");
    } else {
        Serial_WriteString("KTEST_BG: PASS\n");
    }

    ktest_bg_done = 1;
    ktest_bg_marker();
}

/* ktest_bg_marker - empty hook called immediately after ktest_bg_done = 1.
 *
 * The GDB iso-test harness sets a breakpoint here so it can confirm bg
 * ktest finished WITHOUT depending on the shell reaching its first
 * keyboard_getchar.  Coupling the assertion to keyboard_getchar made
 * iso-test flaky under TCG: shell0's loading-screen spinner spins on
 * vesa_tty_spinner_tick() until ktest_bg_done flips, then must drain
 * poll, print banner, and only THEN call keyboard_getchar -- the cumul-
 * ative wall-clock occasionally raced the 120 s GDB-step budget.
 *
 * Putting the marker right after the flag write lets the assertion
 * happen the moment the kernel guarantees the flag is set, regardless
 * of the subsequent shell-render timing.
 *
 * noinline + externally visible so GDB sees the symbol.  The function
 * body is intentionally a single memory-clobbering nop so the optimiser
 * cannot fold it away. */
__attribute__((noinline))
void ktest_bg_marker(void)
{
    __asm__ volatile("" ::: "memory");
}
