/*
 * usertest.c — minimal ring-3 smoke test.
 *
 * Spawns a kernel task that builds a one-page user address space, copies a
 * tiny position-independent binary into it, and drops to ring 3 via
 * ring3_enter().  The user code calls SYS_WRITE to print a greeting and then
 * SYS_EXIT to terminate cleanly.
 */

#include <kernel/ring3.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/task.h>
#include <kernel/descr_tbl.h>
#include <kernel/tty.h>
#include <string.h>

/* User virtual address layout.
 * Both addresses sit above the 256 MiB kernel identity window (PDE 64+). */
#define USER_CODE_BASE   0x40000000u   /* user code mapped here         */
#define USER_STACK_TOP   0xBFFF0000u   /* user stack top; one page below */

/*
 * Position-independent i386 machine code with debug checkpoints:
 *
 *   SYS_DEBUG(1);                       // CP 1: entered ring-3
 *   write("Welcome to userspace!\n");   // CP 2: if printed, write syscall works
 *   SYS_DEBUG(2);                       // CP 2: returned from write
 *   exit(0);
 *
 * Disassembly (offsets from code start):
 *   00: B8 64 00 00 00   mov   eax, 100  ; SYS_DEBUG
 *   05: BB 01 00 00 00   mov   ebx, 1    ; checkpoint 1
 *   0A: CD 80            int   0x80
 *   0C: E8 00 00 00 00   call  +0        ; push &pop_esi, jmp to pop_esi
 *   11: 5E               pop   esi       ; esi = runtime addr of this insn
 *   12: 83 C6 22         add   esi, 0x22 ; esi = &msg  (0x33 - 0x11 = 0x22)
 *   15: B8 04 00 00 00   mov   eax, 4    ; SYS_WRITE
 *   1A: 89 F3            mov   ebx, esi
 *   1C: CD 80            int   0x80
 *   1E: B8 64 00 00 00   mov   eax, 100  ; SYS_DEBUG
 *   23: BB 02 00 00 00   mov   ebx, 2    ; checkpoint 2
 *   28: CD 80            int   0x80
 *   2A: B8 01 00 00 00   mov   eax, 1    ; SYS_EXIT
 *   2F: 31 DB            xor   ebx, ebx
 *   31: CD 80            int   0x80
 *   33: msg              "Welcome to userspace!\n\0"
 */
static const uint8_t user_test_bin[] = {
    0xB8, 0x64, 0x00, 0x00, 0x00,          /* mov   eax, 100    */
    0xBB, 0x01, 0x00, 0x00, 0x00,          /* mov   ebx, 1      */
    0xCD, 0x80,                             /* int   0x80  (CP1) */
    0xE8, 0x00, 0x00, 0x00, 0x00,          /* call  +0          */
    0x5E,                                   /* pop   esi         */
    0x83, 0xC6, 0x22,                       /* add   esi, 0x22   */
    0xB8, 0x04, 0x00, 0x00, 0x00,          /* mov   eax, 4      */
    0x89, 0xF3,                             /* mov   ebx, esi    */
    0xCD, 0x80,                             /* int   0x80  (write)*/
    0xB8, 0x64, 0x00, 0x00, 0x00,          /* mov   eax, 100    */
    0xBB, 0x02, 0x00, 0x00, 0x00,          /* mov   ebx, 2      */
    0xCD, 0x80,                             /* int   0x80  (CP2) */
    0xB8, 0x01, 0x00, 0x00, 0x00,          /* mov   eax, 1      */
    0x31, 0xDB,                             /* xor   ebx, ebx    */
    0xCD, 0x80,                             /* int   0x80  (exit)*/
    /* msg at offset 0x33: */
    'W','e','l','c','o','m','e',' ',
    't','o',' ',
    'u','s','e','r','s','p','a','c','e','!','\n','\0'
};

static void usertest_task(void)
{
    /* 1. Fresh user address space (kernel PDEs shared). */
    uint32_t *pd = vmm_create_pd();
    if (!pd) {
        t_writestring("ring3test: vmm_create_pd failed\n");
        return;
    }

    /* 2. Code page — user-readable, not writable. */
    uint32_t code_phys = pmm_alloc_frame();
    if (code_phys == PMM_ALLOC_ERROR) {
        t_writestring("ring3test: pmm_alloc_frame failed (code)\n");
        vmm_free_pd(pd);
        return;
    }
    memset((void *)code_phys, 0, PMM_FRAME_SIZE);
    memcpy((void *)code_phys, user_test_bin, sizeof(user_test_bin));
    vmm_map_page(pd, USER_CODE_BASE, code_phys, VMM_FLAG_USER);

    /* 3. Stack page — user-readable and writable. */
    uint32_t stack_phys = pmm_alloc_frame();
    if (stack_phys == PMM_ALLOC_ERROR) {
        t_writestring("ring3test: pmm_alloc_frame failed (stack)\n");
        pmm_free_frame(code_phys);
        vmm_free_pd(pd);
        return;
    }
    memset((void *)stack_phys, 0, PMM_FRAME_SIZE);
    vmm_map_page(pd, USER_STACK_TOP - PMM_FRAME_SIZE,
                 stack_phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /* 4. Activate the user address space and point the TSS at our kernel stack
     *    so that int 0x80 from ring 3 lands in the right place. */
    task_current()->page_dir = pd;
    tss_set_kernel_stack((uint32_t)(task_current()->stack + TASK_STACK_SIZE));
    vmm_switch(pd);

    /* 5. Drop to ring 3 — does not return. */
    ring3_enter(USER_CODE_BASE, USER_STACK_TOP);
}

void cmd_ring3test(void)
{
    t_writestring("Spawning ring-3 test process...\n");
    task_t *t = task_create("ring3test", usertest_task);
    if (!t) {
        t_writestring("ring3test: task_create failed (pool full?)\n");
        return;
    }
    /* Yield so the new task runs before the shell blocks on keyboard input.
       This is a cooperative scheduler — without an explicit yield here the
       ring3test task would never get CPU time until the next keypress. */
    task_yield();
}
