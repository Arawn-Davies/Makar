/*
 * usertest.c - minimal ring-3 smoke test.
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
 * Position-independent i386 machine code with debug checkpoints.
 * Uses the Linux i386 ABI: write(fd, buf, len) via int 0x80.
 *
 *   SYS_DEBUG(1);
 *   write(1, "Welcome to userspace!\n", 22);
 *   SYS_DEBUG(2);
 *   exit(0);
 *
 * Disassembly (offsets from code start, esi = runtime addr of byte 0x11):
 *   00: B8 64 00 00 00      mov  eax, 100          ; SYS_DEBUG
 *   05: BB 01 00 00 00      mov  ebx, 1            ; checkpoint 1
 *   0A: CD 80               int  0x80
 *   0C: E8 00 00 00 00      call +0                ; esi ← &byte[0x11]
 *   11: 5E                  pop  esi
 *   12: B8 04 00 00 00      mov  eax, 4            ; SYS_WRITE
 *   17: BB 01 00 00 00      mov  ebx, 1            ; fd = stdout
 *   1C: 8D 8E 2D 00 00 00   lea  ecx, [esi+0x2D]  ; buf = &msg (0x3E-0x11=0x2D)
 *   22: BA 16 00 00 00      mov  edx, 22           ; len = 22
 *   27: CD 80               int  0x80
 *   29: B8 64 00 00 00      mov  eax, 100          ; SYS_DEBUG
 *   2E: BB 02 00 00 00      mov  ebx, 2            ; checkpoint 2
 *   33: CD 80               int  0x80
 *   35: B8 01 00 00 00      mov  eax, 1            ; SYS_EXIT
 *   3A: 31 DB               xor  ebx, ebx
 *   3C: CD 80               int  0x80
 *   3E: msg                 "Welcome to userspace!\n\0"
 */
static const uint8_t user_test_bin[] = {
    /* SYS_DEBUG(1) */
    0xB8, 0x64, 0x00, 0x00, 0x00,          /* 00: mov  eax, 100         */
    0xBB, 0x01, 0x00, 0x00, 0x00,          /* 05: mov  ebx, 1           */
    0xCD, 0x80,                             /* 0A: int  0x80  (CP1)      */
    /* get runtime address */
    0xE8, 0x00, 0x00, 0x00, 0x00,          /* 0C: call +0               */
    0x5E,                                   /* 11: pop  esi              */
    /* SYS_WRITE(1, &msg, 22) */
    0xB8, 0x04, 0x00, 0x00, 0x00,          /* 12: mov  eax, 4           */
    0xBB, 0x01, 0x00, 0x00, 0x00,          /* 17: mov  ebx, 1  (stdout) */
    0x8D, 0x8E, 0x2D, 0x00, 0x00, 0x00,   /* 1C: lea  ecx, [esi+0x2D]  */
    0xBA, 0x16, 0x00, 0x00, 0x00,          /* 22: mov  edx, 22          */
    0xCD, 0x80,                             /* 27: int  0x80  (write)    */
    /* SYS_DEBUG(2) */
    0xB8, 0x64, 0x00, 0x00, 0x00,          /* 29: mov  eax, 100         */
    0xBB, 0x02, 0x00, 0x00, 0x00,          /* 2E: mov  ebx, 2           */
    0xCD, 0x80,                             /* 33: int  0x80  (CP2)      */
    /* SYS_EXIT(0) */
    0xB8, 0x01, 0x00, 0x00, 0x00,          /* 35: mov  eax, 1           */
    0x31, 0xDB,                             /* 3A: xor  ebx, ebx         */
    0xCD, 0x80,                             /* 3C: int  0x80  (exit)     */
    /* msg at offset 0x3E: "Welcome to userspace!\n\0" */
    'W','e','l','c','o','m','e',' ',
    't','o',' ',
    'u','s','e','r','s','p','a','c','e','!','\n','\0'
};

void ring3_usertest_task(void)
{
    /* 1. Fresh user address space (kernel PDEs shared). */
    uint32_t *pd = vmm_create_pd();
    if (!pd) {
        t_writestring("ring3test: vmm_create_pd failed\n");
        return;
    }

    /* 2. Code page - user-readable, not writable. */
    uint32_t code_phys = pmm_alloc_frame();
    if (code_phys == PMM_ALLOC_ERROR) {
        t_writestring("ring3test: pmm_alloc_frame failed (code)\n");
        vmm_free_pd(pd);
        return;
    }
    memset((void *)code_phys, 0, PMM_FRAME_SIZE);
    memcpy((void *)code_phys, user_test_bin, sizeof(user_test_bin));
    vmm_map_page(pd, USER_CODE_BASE, code_phys, VMM_FLAG_USER);

    /* 3. Stack page - user-readable and writable. */
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

    /* 5. Drop to ring 3 - does not return. */
    ring3_enter(USER_CODE_BASE, USER_STACK_TOP);
}

void cmd_ring3test(int argc, char **argv)
{
    (void)argc; (void)argv;
    t_writestring("Spawning ring-3 test process...\n");
    task_t *t = task_create("ring3test", ring3_usertest_task);
    if (!t) {
        t_writestring("ring3test: task_create failed (pool full?)\n");
        return;
    }
    /* Yield so the new task runs before the shell blocks on keyboard input.
       This is a cooperative scheduler - without an explicit yield here the
       ring3test task would never get CPU time until the next keypress. */
    task_yield();
}
