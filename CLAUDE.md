# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Makar is a hobby x86 (i386) bare-metal OS kernel written in C and AT&T assembly, booted via GRUB Multiboot 2. It targets 32-bit protected mode and runs in QEMU. Docker wraps the full build/test toolchain — no host cross-compiler is required.

## Build commands

```sh
./docker-iso.sh          # build ISO inside Docker (recommended, no toolchain needed)
./docker-qemu.sh         # build + run in QEMU headless (serial → serial.log)
./docker-ktest.sh        # build TEST_MODE ISO, run ktest suite, QEMU exits when done
./docker-test.sh         # full suite: ktest + GDB boot tests

./build.sh               # build without Docker (needs i686-elf-gcc cross-toolchain)
./iso.sh                 # create bootable ISO (calls build.sh + grub-mkrescue)
./clean.sh               # remove build artifacts

# Docker Compose equivalents
docker compose run --rm build          # release ISO
docker compose run --rm build-debug    # debug ISO (-O0 -g3)
docker compose run --rm test           # debug build + headless boot test
```

Debug builds use `-O0 -g3`; release uses `-O2 -g`. Override via `CFLAGS`.
Test mode uses `-DTEST_MODE`: kernel runs `ktest_run_all()` then exits QEMU via `isa-debug-exit`.

## Testing

**In-kernel test suite** (fastest, QEMU exits when done):
```sh
./docker-ktest.sh        # builds with -DTEST_MODE, runs ktest, exits cleanly
# output: ktest.log; exits 0 on pass, 1 on fail
```

**GDB boot-test suite** (comprehensive):
```sh
./test-gdb.sh            # native (needs i686-elf-gdb or gdb-multiarch)
./docker-test.sh         # Docker: ktest suite + GDB boot tests
```
GDB test script: `tests/gdb_boot_test.py`. Connects to `:1234`, verifies Multiboot 2 magic, symbol addresses, memory layout, boot checkpoints. Output: `gdb-test.log`.

**Interactive GDB debug**:
```sh
./gdb.sh                 # Terminal 1: starts QEMU with -s -S (waits for GDB)
# Terminal 2:
i686-elf-gdb src/kernel/makar.kernel -ex "target remote :1234" -ex "break kernel_main" -ex "continue"
```

GDB auto-detect order: `i686-elf-gdb` → `gdb-multiarch` → `gdb`.

**In-kernel test suite (interactive)**: shell command `ktest` runs all suites from the kernel shell.

## Architecture

### Boot sequence (`kernel_main`)
1. `terminal_initialize` → `init_serial(COM1)` → `init_descriptor_tables` (GDT+IDT)
2. Exception handlers, PMM, paging (256 MiB identity map, 4 MiB pages), heap
3. VESA, timer (50 Hz PIT), keyboard, IDE/VFS
4. `tasking_init` + `task_create("shell", shell_run)`
5. `syscall_init` (registers `int 0x80` handler at IDT gate DPL=3)
6. Idle loop: `task_yield` + `hlt`

### Memory map
- `0x00000000–0x0FFFFFFF` (256 MiB): kernel identity window (4 MiB large pages)
- `0x40000000` (`USER_CODE_BASE`): ring-3 code page
- `0xBFFF0000` (`USER_STACK_TOP`): ring-3 stack top (one 4 KiB page below)

### Tasking
Cooperative round-robin scheduler — no preemption, timer tick only advances accounting. `task_yield()` → context switch via `task_asm.S`. `task_exit()` marks the task DEAD and yields. Pool is fixed-size.

### Syscall ABI (`int 0x80`, Linux i386 convention)
| EAX | Syscall   | Args |
|-----|-----------|------|
| 1   | SYS_EXIT  | — |
| 4   | SYS_WRITE | EBX = NUL-terminated string ptr |
| 100 | SYS_DEBUG | EBX = uint32 checkpoint value (prints to VGA + serial) |
| 158 | SYS_YIELD | — |

### VMM (per-task page directories)
- `vmm_create_pd()` — allocates a page directory and mirrors kernel PDEs (indices 0–63)
- `vmm_map_page(pd, vaddr, paddr, flags)` — installs 4 KiB mapping; creates page tables on demand with `PAGE_USER`
- `vmm_switch(pd)` — loads CR3

### Ring-3 entry (`ring3.S`)
`ring3_enter(entry, stack_top)` loads user data selector (0x23) into DS/ES/FS/GS, builds a 5-word `iret` frame (SS=0x23, ESP, EFLAGS|IF, CS=0x1B, EIP), and executes `iret`. Never returns. Caller must call `tss_set_kernel_stack()` and `vmm_switch(pd)` first.

### Ring-3 smoke test (`ring3test` shell command)
Shell command `ring3test` → `task_create("ring3test", usertest_task)` → `task_yield`.

`usertest_task` sets up a VMM page directory, maps code (USER_CODE_BASE, user-RO) and stack (USER_STACK_TOP-4K, user-RW) pages, activates the PD, sets the TSS kernel stack, then calls `ring3_enter`.

The embedded PIC binary (`user_test_bin` in `usertest.c`) executes:
`SYS_DEBUG(1)` → `SYS_WRITE("Welcome to userspace!\n")` → `SYS_DEBUG(2)` → `SYS_EXIT(0)`.

To diagnose failures: watch `serial.log` for `[ring3] CP: 0x1` and `[ring3] CP: 0x2`. CP1 appears before any syscall; CP2 appears after the write but before exit. If CP1 appears but not CP2, the write syscall is the fault; if neither appears, ring-3 entry itself failed.

## Debug output
- VGA: `t_writestring`, `t_hex`, `t_dec`, `t_putchar` (`include/kernel/tty.h`)
- Serial: `Serial_WriteString`, `Serial_WriteHex` (`include/kernel/serial.h`)
- `KLOG` / `KLOG_HEX` macros — serial only, require `-DDEV_BUILD` compile flag (no-ops in release)
- `SYS_DEBUG` writes to **both** VGA and serial unconditionally — preferred for ring-3 debugging

## Key source layout
```
src/kernel/arch/i386/
  boot/       boot.S (Multiboot 2 entry), crti/crtn
  core/       GDT/IDT (descr_tbl.c), ISR stub (isr_asm.S), interrupt dispatch (isr.c)
  mm/         pmm.c (frame allocator), paging.c, vmm.c (per-task page dirs), heap.c
  drivers/    serial, keyboard, timer, IDE, ACPI, partition
  fs/         fat32.c, iso9660.c, vfs.c
  display/    tty.c (VGA text), vesa.c + vesa_tty.c (VESA framebuffer)
  proc/       task.c + task_asm.S (scheduler), syscall.c, ring3.S, usertest.c, ktest.c
  shell/      shell.c, shell_cmds.c, shell_help.c
  debug/      exception handlers (INT 1/3/8/13/14, serial-first output)
src/kernel/kernel/kernel.c   kernel_main
src/kernel/include/kernel/   all public headers
src/libc/                    minimal freestanding libc (string, stdio, stdlib) → libk.a
tests/                       GDB boot-test suite (gdb_boot_test.py) + test groups
```

## Active branch: `feat/vmm-ring3-userspace`
Current focus: debugging ring-3 userspace smoke test output. See the ring-3 smoke test section above for checkpoint interpretation.
