# Project Layout

Makar's source tree is split between **kernelspace** (Ring 0) and the
future **userspace** (Ring 3), mirroring the structure used in
[Medli](https://github.com/Arawn-Davies/Medli).

---

## `src/kernel/` - Kernelspace (Ring 0)

The bare-metal kernel, compiled with `-ffreestanding` and linked against
`libk`.  Architecture-specific code lives under `arch/<arch>/` and is
further divided into logical groups:

| Subdirectory | Contents |
|---|---|
| `arch/i386/boot/` | Early boot (`boot.S`), C-runtime stubs (`crti.S` / `crtn.S`) |
| `arch/i386/core/` | CPU descriptor tables (GDT/IDT), ISR / IRQ dispatch |
| `arch/i386/mm/` | Physical memory manager, paging, VMM, kernel heap |
| `arch/i386/drivers/` | Hardware drivers: serial, timer, keyboard, IDE, ACPI, partition |
| `arch/i386/fs/` | Filesystem layer: FAT32, ISO9660, VFS |
| `arch/i386/display/` | VGA text terminal, VESA linear framebuffer + TTY |
| `arch/i386/proc/` | Scheduler, syscall layer, ring-3 entry, usertest, ktest runner |
| `arch/i386/shell/` | Interactive kernel shell and built-in commands |
| `arch/i386/debug/` | Exception handlers (page fault, GPF, double fault, breakpoints) |
| `kernel/` | Architecture-independent entry point (`kernel.c` / `kernel_main`) |
| `include/kernel/` | Public kernel headers |

---

## `src/libc/` - C Standard Library

A freestanding C library built in two flavours:

* **`libk`** - kernel variant (`-D__is_libk`), linked into the kernel.
* **`libc`** - hosted variant (not yet built), intended for future userspace.

Provides `stdio`, `stdlib`, and `string` primitives.

---

## `src/userspace/` - Userspace (Ring 3)

*Placeholder - not yet built.*

This is the future home of Ring-3 programs (init, shell, user utilities).
Once the kernel exposes a stable syscall ABI and ELF loader, user programs
will be added here and linked against `libc`.

See [`src/userspace/README.md`](src/userspace/README.md) for the planned
sub-structure.

---

## Other top-level directories

| Path | Purpose |
|---|---|
| `docs/` | Subsystem documentation (one `.md` per module) |
| `tests/` | Automated GDB boot-test suite (`gdb_boot_test.py` + groups) |
| `LICENSES/` | Third-party licence texts |

---

## Top-level scripts

| Script | Purpose |
|---|---|
| `build.sh` | Build all projects into the sysroot |
| `iso.sh` | Build `makar.iso` via `grub-mkrescue` |
| `qemu.sh` | Build and run in QEMU (CD-ROM + HDD) |
| `qemu-hdd.sh` | Boot directly from `hdd.img` (no CD-ROM) |
| `docker-iso.sh` | Build `makar.iso` in the CI Docker image |
| `docker-ktest.sh` | Build TEST_MODE ISO, run ktest suite, QEMU exits when done |
| `docker-test.sh` | Build in Docker, run ktest suite + GDB boot test suite |
| `docker-qemu.sh` | Build in Docker, run interactively in host QEMU |
| `gdb.sh` | Build and launch with GDB stub on `:1234` |
| `test-gdb.sh` | Local (non-Docker) GDB boot test suite |
| `clean.sh` | Remove build artefacts |
| `mkhdd.sh` | Create a raw hard-disk image |

---

## Docker files

| File | Purpose |
|---|---|
| `Dockerfile` | Extends `arawn780/gcc-cross-i686-elf:fast` (the CI image) to create a self-contained build environment with the i686-elf cross-toolchain, GRUB tools, QEMU, and GDB pre-installed |
| `docker-compose.yml` | Defines three services - `build` (release ISO), `build-debug` (debug ISO), and `test` (debug build + headless serial smoke test) - all bind-mounting the source tree so output lands in your checkout |
