# CLAUDE.md

Guidance for Claude Code working in this repo.

> ## ⛔ DO NOT ADD CONTENT TO THIS FILE
> CLAUDE.md is an **index/router**, not a manual. Its only job is to point you
> to where the real information lives (`docs/`, `SURVEY.md`, the companions
> below, and the source). When you learn or change something, **write it in the
> appropriate doc/companion/source and, if needed, add or update a one-line
> pointer here** — never paste the detail into this file. If a section here is
> growing past a few lines, that content belongs somewhere else. Keep it short.

## What this is

Makar is a hobby x86 (i386) bare-metal OS kernel in C + AT&T asm, booted via
GRUB Multiboot 2, 32-bit protected mode, runs in QEMU. Docker wraps the full
build/test toolchain — no host cross-compiler required.

## Build & run (`./run.sh`)

```sh
./run.sh iso boot       # debug ISO → interactive QEMU
./run.sh iso test       # regression gate: ktest + in-guest script drivers + GDB checkpoints
./run.sh iso build      # kernel + makar.iso + makar-test.iso, no run
./run.sh iso release    # optimised release ISO
./run.sh hdd boot|test|build|release
./run.sh ktest          # fast kernel-only ktest (headless, streams serial)
./run.sh kbtest [gui]   # in-guest key-injection test (serial KBTEST markers)
./run.sh gdb iso|hdd    # GDB boot test against an existing image
./run.sh clean
```

- Debug `-O0 -g3`, release `-O2 -g`; override via `CFLAGS`.
- `run.sh` execution context, in order: `/.dockerenv` (CI) → run directly;
  Docker CLI → wrap in `docker run`; host `i686-elf-gcc` → run directly; else
  error. QEMU/GDB steps prefer host binaries when present.
- **Single kernel, two ISOs**: one `makar.kernel`; `iso.sh` packages `makar.iso`
  (interactive) and `makar-test.iso` (`test_mode` cmdline). No compile-time test
  flag. `build.sh` uses `-j$(nproc)` + ccache (`CCACHE=0` to disable).
- KVM: default ON for interactive/perf runs (`iso`/`hdd boot`, `guitest`) when
  `/dev/kvm` is usable and not in CI; forced OFF (TCG) for the correctness gates
  (`ktest`, `gdb`, `nettest`) and all CI, because it broke GDB breakpoints +
  masked a ktest fault under TCG. `MAKAR_USE_KVM=1`/`=0` overrides either way.

## Testing — in-guest and headless only

**No host-driven keyboard or monitor input, no framebuffer scraping — ever.**
The old HMP/`sendkey` harness is gone. Two mechanisms, both asserting on COM1
serial markers:

1. **Script drivers** (no keyboard), run by `./run.sh iso test` after
   `ktest_run_all()`: `src/userspace/{shell-smoke,incore,libc-tcc}.sh`. Each is
   an in-guest sh script gating per-test PASS/FAIL on `$?`; markers
   `SHELL-SMOKE/INCORE/LIBC-TCC: ALL PASS`. Drive the ring-3 shell with
   `sh.elf -c '<payload>'`. `alloctest.c` is the canonical ELF shape.
2. **Key injection** (`./run.sh kbtest`) for paths where the keystroke itself is
   under test: `keyboard_inject_text/_key()` feed the live decode→ring→shell
   pipeline; `keyboard_test_driver()` (cmdline `kbtest`) emits `KBTEST:` markers.

Add coverage in the `.sh` drivers or `keyboard_test_driver()` — never a host
harness. Full detail: **`docs/testing.md`**.

## Architecture (pointers — read the source for detail)

**Boot (`kernel_main`)**: descriptor tables → `fpu_init` (x87 armed + per-task
`fxsave`/`fxrstor` on context switch) → exceptions/PMM/paging/heap → VESA (720p
Bochs VBE, else VGA 80×50) → timer(100Hz)/keyboard/IDE → cmdline parse
(`test_mode`, `console=ttyS0`, `root=<spec>`) → `vfs_init`/`vfs_mount_root`/
`vfs_auto_mount` → `tasking_init` + shell/ktest tasks → `syscall_init` → idle.

**Memory map** (higher-half kernel): kernel linked at `0xC0000000` (loaded at
phys 1 MiB via `AT()`); runtime page directory keeps `0x0–0x0FFFFFFF` as a low
identity window (4 MiB pages) for phys access (PMM frames, MBI, framebuffer,
ACPI) alongside the `0xC0000000+` high kernel map. User space (below
`0xC0000000`): `0x40000000` ring-3 code; `0x90000000` anon-mmap window;
`0xBFFF0000` ring-3 stack top (`USER_STACK_PAGES=8`). Hand any kernel pointer to
DMA/hardware via `kvirt_to_phys()` (`kernel/paging.h`). TCC in-OS build links
low-half (`KERNEL_VBASE=0`).

**Subsystems** (source is the source of truth):
- Tasking/scheduler, per-task `task_t`: `kernel/task.h`, `proc/task.c`
  (round-robin, PIT preempt `SCHED_QUANTUM=4`, ring-3 faults → SIGSEGV not panic).
- VFS mount table (`s_mounts[]`, longest-prefix routing, ext2/FAT32/ISO9660 +
  `/dev` `/proc` `/tmp` `/log` overlays): `fs/vfs.c`.
- Keyboard layered driver (PS/2→scancode→keycode→ASCII/sentinel→per-task ring):
  `drivers/keyboard.c`, `docs/kernel/`.
- Syscall ABI (`int 0x80`, Linux i386): `kernel/syscall.h` (numbers),
  `src/userspace/syscall.h` (wrappers), **`docs/syscalls.md`** (table).
- Two shells: in-kernel `shell/shell.c` (+ `shell_cmd_*.c`) and ring-3
  `src/userspace/sh.c` (freestanding). Scripting: **`docs/scripting.md`**.
- VMM per-task page dirs: `mm/vmm.c`. Ring-3 entry: `proc/ring3.S`.
- Apps inventory: **`SURVEY.md`**. ktest macros: `kernel/ktest.h`.

## Debug output
- VGA: `t_writestring`/`t_hex`/`t_dec` (`kernel/tty.h`). Serial:
  `Serial_WriteString`/`Serial_WriteHex` (`kernel/serial.h`).
- `KLOG`/`KLOG_HEX` — serial only, need `-DDEV_BUILD`. `SYS_DEBUG` — VGA+serial,
  preferred for ring-3 debugging. `kpanic`/`KPANIC`/`kpanic_at` — panic screen.

## Key source layout
```
src/kernel/arch/i386/{boot,core,mm,drivers,fs,display,proc,shell,debug}/
src/kernel/kernel/kernel.c   kernel_main
src/kernel/include/kernel/   public headers
src/libc/                    freestanding libc → libk.a
src/userspace/               ring-3 ELF apps + the hosted libc.a
toolchain/                   host musl cross toolchain (separate; see its README)
tests/                       GDB boot-test suite
```

## Companion docs
- **`CLAUDE.history.md`** — shipped state, PR log, FOSS attribution.
- **`CLAUDE.roadmap.md`** — slice queue + porting/hardware roadmap.
- `docs/` — `testing.md`, `posix.md`, `syscalls.md`, `scripting.md`,
  `internals.md`, `gui.md` (windowing/compositor + shared surfaces), `kernel/`;
  `SURVEY.md`; `toolchain/README.md`.

## Conventions
- Full house rules + style: the **`makar-conventions`** Claude skill
  (`.claude/skills/makar-conventions/SKILL.md`) — read it before non-trivial work.
- Paths follow Linux: `/usr` `/apps` `/root` `/proc` `/dev` `/mnt/<name>`
  `/mnt/cdrom`; apps at `/apps/*.elf`, libc at `/usr/lib/libc.a`.
- **Commits**: one per discrete work item; **no `Co-Authored-By`, no "Generated
  with Claude Code", no AI-attribution footers** of any kind.
- **Tests**: no host keyboard — add coverage to the `.sh` drivers or
  `keyboard_test_driver()` (see Testing above).
- IDE clang errors about missing kernel headers are expected; the build uses the
  right include paths.
