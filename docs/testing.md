# Testing Makar

This guide covers the automated test infrastructure. For build and run
instructions see [Building & Running](building.md).

---

## CI test suite (`./run.sh iso-test`)

The single command for complete ISO CI validation:

```sh
./run.sh iso-test
```

Runs two phases; build steps use Docker, QEMU/GDB prefer the host if available:

**Phase 1 - in-kernel ktest suite**

Builds an ISO whose grub.cfg passes the `test_mode` kernel argument. The
kernel parses the multiboot2 cmdline, runs `ktest_run_all()` (all subsystem
unit tests including a live ring-3 userspace execution), then exits QEMU
cleanly via `isa-debug-exit`. Output: `ktest.log`.

**Phase 2 - GDB boot-checkpoint tests**

Builds a normal debug ISO. Creates a 32 MiB FAT32 test disk and attaches it
on IDE:0 alongside the CD-ROM so the kernel can mount `/hd`. Launches QEMU
with the GDB stub and runs `tests/gdb_boot_test.py`. Output: `gdb-test.log`.

Exit code 0 = everything passed; 1 = any failure or timeout.

---

## HDD boot test (`./run.sh hdd-test`)

Verifies the installed HDD boot path end-to-end - no CD-ROM attached:

```sh
./run.sh hdd-test
```

What it does:

1. **Clean rebuild** - ensures `src/kernel/makar.kernel` (GDB symbol file) matches the binary written into the image.
2. **Generate `makar-hdd-test.img`** - fresh raw MBR + FAT32 + GRUB 2 image using the interactive kernel so `shell_run` is called and `vfs_auto_mount()` runs. Kept separate from `makar-hdd.img` so interactive and test images never share state.
3. **GDB boot test** - boots the image with `-boot c` (HDD-only) and runs `tests/gdb_hdd_test.py`.

Output files: `hdd-test-gdb.log`, `hdd-test-serial.log`.

---

## GDB test groups

Both `gdb_boot_test.py` (ISO boot) and `gdb_hdd_test.py` (HDD boot) run the
same four groups, providing equivalent external verification regardless of
boot medium:

| Group | What it verifies |
|---|---|
| `boot_checkpoints` | Every major boot function reached in order: `kernel_main` → `terminal_initialize` → … → `shell_run` |
| `hardware_state` | CR0.PG set (paging enabled), CR3 non-zero (page directory loaded), `timer_callback` fires (PIT ticking) |
| `vesa` | VESA framebuffer active and TTY initialised (or absent without crashing - graceful headless) |
| `hdd_mount` | `fat32_mounted()` non-zero - FAT32 partition auto-mounted at `/hd` after `shell_run` |

The `hdd_mount` check advances execution to `keyboard_getchar` (the shell's
read-loop entry) before inspecting `fat32_mounted()`, ensuring
`vfs_auto_mount()` has fully completed.

The ISO GDB test creates the FAT32 test disk using `mkfs.fat --offset`
(sector-based, no losetup / `--privileged` needed), which works inside the
GitHub Actions container job.

To add a new group: create `tests/groups/<name>.py` exposing `NAME` and
`run() → bool`, then import it into **both** `gdb_boot_test.py` and
`gdb_hdd_test.py`.

---

## Interactive GDB debug

```sh
# Build a debug ISO first
./run.sh iso-boot   # or: CFLAGS='-O0 -g3' ./run.sh iso-release

# In one terminal - start QEMU with GDB stub (inside Docker)
docker run --rm -it -v "$PWD:/work" -w /work arawn780/gcc-cross-i686-elf:fast \
    bash -lc 'qemu-system-i386 -cdrom makar.iso -s -S -display none -serial stdio'

# In another terminal - attach GDB (inside Docker or native)
docker run --rm -it -v "$PWD:/work" -w /work arawn780/gcc-cross-i686-elf:fast \
    gdb-multiarch src/kernel/makar.kernel \
        -ex "target remote :1234" \
        -ex "break kernel_main" \
        -ex "continue"
```

The `-O0 -g3` flags ensure DWARF debug info is accurate. QEMU starts with
`-S` (freeze at reset), giving you time to set breakpoints before execution
begins.

---

## In-kernel unit tests (interactive)

From the kernel shell:

```
ktest
```

Runs `ktest_run_all()` and prints pass/fail for each subsystem suite
(PMM, heap, ring-3 execution, etc.) directly to the terminal and serial log.

---

## CI

GitHub Actions runs both test jobs on every push and PR:

| Job | Runner | What runs |
|---|---|---|
| `iso-test` | `ubuntu-latest` + container `arawn780/gcc-cross-i686-elf:fast` | `./run.sh iso-test` |
| `hdd-test` | `ubuntu-latest` (bare metal, Docker available) | `./run.sh hdd-test` |

The `release.yml` workflow gates artifact publication on both jobs passing.
