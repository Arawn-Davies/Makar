# Makar Kernel - Project Context

## What it is
Makar is a hobby x86 (i386) kernel written in C and AT&T assembly, booted via Multiboot 2 (GRUB).
It targets 32-bit protected mode and runs inside QEMU; Docker wraps the build/test toolchain so no
host toolchain is needed.

## Repo layout
```
src/kernel/
  arch/i386/
    core/       descr_tbl, GDT/IDT, ISR stub (isr_asm.S), interrupt dispatch (isr.c)
    init/       boot.S (Multiboot entry), crti/crtn
    mm/         pmm.c (frame allocator), paging.c (4 MiB identity map), vmm.c (per-task page dirs)
    system/     task.c/task_asm.S (cooperative scheduler), syscall.c, ring3.S, usertest.c, shell*
    hardware/   serial, keyboard, timer, IDE, FAT32, ISO9660, VFS, ACPI
    display/    VGA text mode (tty.c), VESA framebuffer (vesa.c/vesa_tty.c)
    debug/      exception handlers
  include/kernel/   all public headers
  kernel/kernel.c   kernel_main - boot sequence
src/libc/           minimal freestanding libc (string, stdio, stdlib)
tests/              ktest suite (run via `ktest` shell command)
```

## Boot sequence (kernel_main)
1. `terminal_initialize` → `init_serial(COM1)` → `init_descriptor_tables` (GDT+IDT)
2. Exception handlers, PMM, paging (256 MiB identity, 4 MiB pages), heap
3. VESA, timer (50 Hz PIT), keyboard, IDE/VFS
4. `tasking_init` + `task_create("shell", shell_run)`
5. `syscall_init` (registers int 0x80 handler)
6. Idle loop: `task_yield` + `hlt`

## Memory map
- `0x00000000–0x0FFFFFFF` (256 MiB): kernel identity window (4 MiB large pages)
- `0x40000000` (`USER_CODE_BASE`): ring-3 code page
- `0xBFFF0000` (`USER_STACK_TOP`): ring-3 stack top (one 4 KiB page below)

## Tasking
Cooperative round-robin scheduler (no preemption, timer tick just advances accounting).
`task_yield()` triggers a context switch via `task_asm.S`.  `task_exit()` marks the task DEAD
and yields.  Pool is fixed-size.

## Syscall ABI (int 0x80, Linux i386 convention)
| EAX | Syscall      | Args            |
|-----|--------------|-----------------|
| 1   | SYS_EXIT     | -               |
| 4   | SYS_WRITE    | EBX = NUL-term string ptr |
| 100 | SYS_DEBUG    | EBX = uint32 checkpoint value (prints to VGA + serial) |
| 158 | SYS_YIELD    | -               |

`syscall_init()` → `register_interrupt_handler(0x80, syscall_dispatch)`.
The IDT gate is DPL=3 (opened in `init_descriptor_tables`) so ring-3 `int 0x80` is legal.

## VMM (per-task page directories)
`vmm_create_pd()` - allocates a page directory and mirrors the kernel PDEs (0–63).
`vmm_map_page(pd, vaddr, paddr, flags)` - installs a 4 KiB mapping; creates page tables on demand
with `PAGE_USER` so user code can access them.
`vmm_switch(pd)` - loads CR3.

## Ring-3 entry (`ring3_enter`)
- Located: `src/kernel/arch/i386/system/ring3.S`
- Loads user data selector (0x23) into DS/ES/FS/GS, builds a 5-word iret frame
  (SS=0x23, ESP, EFLAGS|IF, CS=0x1B, EIP), executes `iret`. Never returns.
- Caller must: set `tss_set_kernel_stack()` and call `vmm_switch(pd)` first.

## Ring-3 smoke test (`cmd_ring3test` / `usertest.c`)
Shell command `ring3test` → `task_create("ring3test", usertest_task)` → `task_yield`.
`usertest_task`:
1. `vmm_create_pd` + map code page (USER_CODE_BASE, user-RO) + stack page (USER_STACK_TOP-4K, user-RW)
2. Activate pd, set TSS kernel stack
3. `ring3_enter(USER_CODE_BASE, USER_STACK_TOP)`

Embedded binary (`user_test_bin`): PIC i386 bytecode that calls
SYS_DEBUG(1) → SYS_WRITE("Welcome to userspace!\n") → SYS_DEBUG(2) → SYS_EXIT(0).
CP1 fires before any syscall; CP2 fires after the write but before exit - if CP1 appears but
CP2 does not, the write syscall is the fault; if neither appears, ring-3 entry itself failed.

## Debug output
- VGA: `t_writestring`, `t_hex`, `t_dec`, `t_putchar`  (`include/kernel/tty.h`)
- Serial (COM1): `Serial_WriteString`, `Serial_WriteHex`  (`include/kernel/serial.h`)
- `KLOG` / `KLOG_HEX` macros - serial only, require `-DDEV_BUILD` (no-ops in release)
- SYS_DEBUG writes to **both** VGA and serial unconditionally (useful from ring-3)

## Build / run
```sh
./docker-iso.sh          # build ISO inside Docker
./docker-qemu.sh         # run in QEMU (no KVM; serial → serial.log)
./docker-test.sh         # run ktest suite via GDB automation
./build.sh               # build without Docker (needs cross-toolchain on host)
```
Serial output lands in `serial.log`; GDB transcript in `gdb-test.log`.

## Active work (branch: feat/vmm-ring3-userspace)

### What was done this session
- Added exception handlers for #8 (double fault), #13 (GPF), #14 (page fault) in
  `src/kernel/arch/i386/debug/debug.c` - all write to VGA + serial then halt.
- Added `tss_get_esp0()` accessor to `descr_tbl.c/.h` for ktest use.
- Added ktest suites: `"task"`, `"syscall"`, `"gdt"`, `"ring3_prereqs"` in
  `src/kernel/arch/i386/system/ktest.c`.
- `syscall_dispatch` de-static-ified and declared in `syscall.h` so ktest can call it directly.
- Ran a diagnostic build (temporarily auto-ran `ring3test` in `shell_run`) and captured
  `serial.log` - found the root cause (see below). Reverted the diagnostic change.

### Root cause of ring3test freeze (CONFIRMED, NOT YET FIXED)

**Bug 1 - INT 14 handler override:**
`kernel_main` calls `init_debug_handlers()` (which registers our INT 14 handler) and then
`paging_init()` (which also calls `register_interrupt_handler(14, ...)`, overriding ours).
Paging's INT 14 handler only writes to VGA (no serial), then calls `PANIC("Page fault")` which
does `cli + for(;;)`. This disables the PIT timer (spinner stops) and produces no serial output.

File: `src/kernel/arch/i386/mm/paging.c`, function `paging_init()`
Fix: remove the `register_interrupt_handler(14, page_fault_handler)` call from `paging_init()`.
The debug.c handler supersedes it and writes to both VGA and serial.

**Bug 2 - VESA framebuffer not mapped in user PD (ROOT CAUSE of the page fault):**
`vmm_create_pd()` copies only the first 64 kernel PDEs (covering 0x00000000–0x0FFFFFFF, the
256 MiB identity window). The VESA framebuffer is at `0xFD000000` (PDE 1008 in the kernel PD)
and is NOT copied. When a syscall fires from ring 3, the CPU stays on the user PD (CR3 unchanged
by interrupt). Any `t_writestring` call that routes through `vesa_tty` touches `0xFD000000`
→ page fault → paging's handler → cli + panic → freeze.

File: `src/kernel/arch/i386/mm/vmm.c`, constant `KERNEL_PDE_COUNT` and function `vmm_create_pd()`

### Fixes to apply next session

**Fix A - vmm.c:** Change `vmm_create_pd` to copy **all 1024** PDEs from the kernel PD (not just 64).
Update `vmm_free_pd` to skip PDEs that are identical to the kernel PD (shared entries, not
user-owned) so it does not accidentally free the VESA extra_page_tables static pool.

```c
/* vmm_create_pd: copy all 1024 PDEs */
for (uint32_t i = 0; i < 1024; i++)
    pd[i] = kpd[i];

/* vmm_free_pd: skip shared kernel PDEs */
uint32_t *kpd = paging_kernel_pd();
for (uint32_t pdi = 0; pdi < 1024; pdi++) {
    if (pd[pdi] == kpd[pdi])          /* shared with kernel - do not free */
        continue;
    if (!(pd[pdi] & PAGE_PRESENT) || (pd[pdi] & PAGE_LARGE))
        continue;
    /* walk page table, free frames, free page table */
    ...
}
```

**Fix B - paging.c:** Remove the `register_interrupt_handler(14, page_fault_handler)` line from
`paging_init()`. The debug.c handler is the authoritative INT 14 handler.

After both fixes, rebuild and run `ring3test` - expect to see `[ring3] CP: 0x1` and
`[ring3] CP: 0x2` in serial.log, followed by "Welcome to userspace!" on VGA.

### Memory leak (known, low priority)
After `task_exit()` in ring3test, `vmm_free_pd(task->page_dir)` is never called. The user PD,
code page, and stack page leak. Fix: call `vmm_free_pd` from `task_exit` or from the scheduler
when it reaps a DEAD task with a non-kernel PD.
