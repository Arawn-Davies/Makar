# Makar × Medli - Co-operation roadmap

Makar and [Medli](https://github.com/Arawn-Davies/Medli) are two independent
implementations of the same operating system, developed in parallel by the
same author:

| | Makar | Medli |
|---|---|---|
| **Language** | C / C++ | C# / X# |
| **Runtime** | Bare metal (no managed heap) | Cosmos / IL2CPU |
| **Target** | i686, native ELF | x86, Cosmos PE |
| **Kernel type** | Monolithic C kernel | Managed OS kernel |
| **Repo** | [Arawn-Davies/untitled-os](https://github.com/Arawn-Davies/untitled-os) | [Arawn-Davies/Medli](https://github.com/Arawn-Davies/Medli) |

They are **siblings**, not layers.  Makar is not a bootloader or hardware
abstraction for Medli - it is a ground-up re-implementation of the same
operating system idea in a lower-level language, sharing UX conventions,
filesystem layout, service design, and (eventually) binary formats.

---

## Shared identity

Both OSes present the same conceptual environment to a user sitting at a
terminal or serial console:

- A **command shell** with a consistent command vocabulary.
- A **daemon/service model** (Medli uses `Daemon` objects; Makar will follow
  the same model in C).
- A common **filesystem layout** (see below).
- The **VICS text editor** (currently Medli-only; to be ported to Makar).
- A **user account system** (login, root, guest - currently Medli-only).

The goal is that a user familiar with one OS is immediately at home on the
other.

---

## Filesystem layout

Medli defines the canonical directory structure.  Makar will adopt the same
layout once a writable filesystem driver is in place.

```
0:\                     (volume root)
├── Users\
│   └── Guest\
├── root\
├── Apps\
│   ├── Temp\
│   └── x86\            ← native Makar binaries live here
├── System\
│   ├── Data\
│   ├── Logs\
│   ├── Libraries\
│   └── Modules\
└── Temp\
```

The path separator is `\` on both systems, matching the Medli convention.
`Paths.Separator` in Medli is already defined as `@"\"`.

---

## Binary compatibility goals

The aim is for a user-space program to run on either OS without recompilation
where technically feasible.

### Native Makar binaries (`Apps\x86\`)

Makar targets i686 and produces ELF32 executables.  A thin ELF loader (to be
implemented in Makar's kernel) will map and execute these binaries.  Medli
may gain an ELF compatibility shim through `Medli.Plugs` or a native
execution daemon.

### Medli Executable Format (MEF)

Medli already supports two executable formats:

- **COM** - flat binary, loaded at a fixed address, DOS-compatible.
- **MEF** - Medli Executable Format, the native managed format.

COM binaries are the lowest common denominator: a COM loader in Makar can
run simple Medli COM programs, and vice versa.  MEF requires the Cosmos
managed runtime, so it is Medli-only for now.

### Recommended path

1. Implement a COM loader in Makar (simple flat binary execution).
2. Define a shared **MXF (Makar/Medli Executable Format)** - a lightweight
   ELF-inspired format with a common header that both kernels can parse,
   containing native i686 code.
3. Long-term: Medli hosts a Makar-native execution daemon that forks into
   a sandboxed environment to run MXF binaries using its own scheduler.

---

## Planned Makar kernel milestones

These are the kernel features needed before higher-level co-operation with
Medli is practical.

### Near-term

- [x] **Keyboard driver** - PS/2 keyboard via IRQ 1; translate scan codes to
  ASCII.
- [x] **Shell** - minimal interactive command loop over the VGA/VESA terminal,
  matching Medli's command vocabulary.
- [x] **ATA/IDE driver** - PIO-mode disk access (28-bit LBA, read/write,
  `lsdisks` and `readsector` shell commands).
- [x] **MBR partition support** - read and write MBR partition tables; `lspart`
  and `mkpart mbr` shell commands; MDFS type `0xFA` recognised.
- [x] **GPT partition support** - protective MBR detection, full GPT header and
  entry parsing, `mkpart gpt` creation with CRC32-signed headers and
  auto-generated unique partition GUIDs.
- [x] **FAT32 driver** - read/write access to FAT32 volumes; `mount`, `umount`,
  `ls`, `cat`, `cd`, `mkdir`, `mkfs` shell commands; adopts Medli's filesystem
  layout (`Users\`, `Apps\`, `System\` etc.).

### Medium-term

- [ ] **Process model** - a simple round-robin scheduler, `fork`-like
  primitive, and per-process address spaces using the existing paging
  infrastructure.
- [ ] **ELF loader** - load and execute ELF32 static binaries from the FAT32
  filesystem, starting with `Apps\x86\`.
- [ ] **COM loader** - flat binary loader for Medli COM programs.
- [ ] **Serial shell** - run the same shell over the UART, enabling headless
  operation and Medli-style serial daemon interop.
- [ ] **VICS port** - port the VICS text editor from Medli C# to a Makar C
  implementation, sharing the same key bindings and file format.

### Long-term

- [ ] **MXF executable format** - joint specification with Medli; shared
  header, native i686 code section, relocatable.
- [ ] **Network stack** - IP/TCP over a NE2000-compatible NIC (or virtio-net
  in QEMU), enabling the same HTTP/FTP/SSH/Telnet daemon model as Medli.
- [ ] **User accounts** - `root` and `guest` accounts stored in
  `System\Data\usrinfo.sys`, matching the Medli account file format.
- [ ] **Inter-OS file exchange** - agreed-upon config and data file formats
  (plain text, INI-style) readable by both OSes from a shared FAT32 partition.

---

## Shared UX conventions

Both Makar and Medli should implement these user-facing conventions
identically:

| Convention | Medli status | Makar status |
|---|---|---|
| Path separator `\` | ✅ implemented | planned |
| Root volume `0:\` | ✅ implemented | planned (FAT32 driver) |
| Command: `ls` / `dir` | ✅ implemented | ✅ implemented |
| Command: `cd` | ✅ implemented | ✅ implemented |
| VICS text editor | ✅ implemented | planned (port) |
| User login prompt | ✅ implemented | planned |
| Serial console | ✅ implemented | ✅ implemented (read-only) |
| Daemon/service model | ✅ implemented | planned |
| Kernel version string | ✅ `KernelVersion` | ✅ `SHELL_VERSION` |
| Shell prompt (`user@host cwd~>`) | ✅ implemented | ✅ implemented |
| Welcome / ASCII logo | ✅ implemented | ✅ implemented |

---

## Divergences by design

Some differences are intentional and reflect the different natures of the two
kernels:

| Aspect | Makar | Medli |
|---|---|---|
| Memory management | Manual (PMM + paging + heap) | Cosmos GC |
| Interrupt handling | Bare IDT + ISR stubs | Cosmos abstraction |
| Graphics | VESA linear framebuffer (direct pixel) | Cosmos `VGAScreen` / bitmap |
| Build system | Makefile + cross-GCC | .NET SDK + IL2CPU |
| Debug tooling | GDB over QEMU stub | Cosmos debugger / serial |

These divergences do not affect the user-visible behaviour - they are
implementation details hidden beneath the shared shell and filesystem layer.

---

## How to contribute

If you are working on either OS and want to ensure compatibility:

1. **Agree on file formats early** - config files, log formats, and
   executable headers should be designed jointly so neither OS needs to
   change its format later.
2. **Test on QEMU** - both OSes run under `qemu-system-i386`; use `-hda` with
   a shared FAT32 disk image to test filesystem interoperability.
3. **Keep the shell vocabulary in sync** - add new shell commands to both
   implementations, or document which commands are OS-specific.
4. **Use the serial port as the integration test bus** - both OSes write to
   COM1; an automated test harness can validate that both produce the same
   output for the same commands.
