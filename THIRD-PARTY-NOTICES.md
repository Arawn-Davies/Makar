# Third-party notices & FOSS attribution

Makar itself is licensed **BSD-3-Clause-Clear** (see [`LICENSE`](LICENSE)).

It uses and is influenced by the free/open-source projects below. **Vendored**
code keeps its upstream licence verbatim in its own tree; GPL components are
kept as separate, independently-licensed *programs* (e.g. `doom.elf`) and are
**not** linked into the BSD-licensed kernel.

## Vendored / shipped code (licence preserved in-tree)

| Project | Licence | Source | In-tree licence | Used for |
|---------|---------|--------|-----------------|----------|
| **doomgeneric** (ozkl) | GPLv2 | https://github.com/ozkl/doomgeneric | `vendor/doomgeneric/LICENSE` | Portable DOOM port → `doom.elf` (Makar backend in `src/userspace/doomgeneric_makar.c`) |
| **DOOM** (id Software) | GPLv2 | https://github.com/id-Software/DOOM | (as above) | The DOOM engine wrapped by doomgeneric |
| **TinyCC** | LGPLv2.1 | https://repo.or.cz/tinycc.git | `vendor/tinycc/COPYING` | In-OS C compiler `tcc.elf` |
| **lwIP** | BSD-3-Clause | https://github.com/lwip-tcpip/lwip | `vendor/lwip/COPYING` | TCP/IP stack |
| **Limine** (v12.3.0) | BSD-2-Clause | https://github.com/limine-bootloader/limine | `vendor/limine/LICENSE` | BIOS boot binaries the installer deploys |
| **font8x8** (D. Hepper) | Public domain | https://github.com/dhepper/font8x8 | header note | GUI/console 8×8 bitmap font (`src/userspace/font8x8.h`) |

## Influence / reference (no code copied)

| Project | Licence | Source | Influence |
|---------|---------|--------|-----------|
| **Linux kernel** | GPLv2 | https://github.com/torvalds/linux | Syscall ABI (i386 int 0x80), ELF loading, process memory layout, ACPI soft-off (XSDT/X_DSDT + `\_S5_`) behaviour, per-hypervisor power-off model |
| **ELKS** | GPLv2 | https://github.com/ghaerr/elks | Minimal libc / crt0 model; `vix` editor philosophy; NX-windowing (roadmap) |
| **FUZIX** | GPLv2 | https://github.com/EtchedPixels/FUZIX | vi-style editor design; small-system libc porting approach |
| **CP/M** | DRI (open-sourced 2022) | http://www.cpm.z80.de/ | Terminal-owns-screen philosophy; self-contained program model |
| **musl libc** | MIT | https://musl.libc.org | Target libc for a future hosted userspace; syscall stub conventions |
| **GRUB** | GPLv2 | https://www.gnu.org/software/grub/ | Bootloader; Multiboot 2 tag format |
| **OSDev wiki** | CC-BY-SA | https://wiki.osdev.org | OS-specific toolchain, paging, descriptor tables, ACPI guidance |

Corrections welcome — open an issue if any attribution or licence here is wrong
or incomplete.
