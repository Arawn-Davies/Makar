# Thanks and Acknowledgements

Makar draws on the ideas, documentation, and design of many free and
open-source projects.  No code from these projects is incorporated into Makar
unless explicitly noted in the relevant source file.  Where design decisions
were directly informed by a project's source, that is called out below.

---

## Operating systems and kernels

### Linux kernel
**Licence:** GPLv2  
**URL:** https://kernel.org

The Linux i386 `int 0x80` syscall ABI is used verbatim: syscall numbers,
register conventions (EAX=nr, EBX/ECX/EDX=args), and the `iret`-based
ring-3 entry model are all taken from Linux.  No Linux source code is
present in this repository.

### ELKS (Embeddable Linux Kernel Subset)
**Licence:** GPLv2  
**URL:** https://github.com/ghaerr/elks

ELKS's approach to a minimal `crt0.S` + static freestanding libc on real
hardware directly shaped `src/userspace/`.  VIX's philosophy of a
lightweight, heap-free editor that owns the terminal is modelled on ELKS's
`vi`.  No ELKS source code is present in this repository.

### FUZIX
**Licence:** GPLv2  
**URL:** https://github.com/EtchedPixels/FUZIX

Alan Cox's Unix-like OS for small systems.  FUZIX's vi design and its
approach to portable libc stubs across diverse hardware influenced the VIX
editor and the userspace syscall stub layout.  No FUZIX source code is
present in this repository.

### xv6 (MIT)
**Licence:** MIT  
**URL:** https://github.com/mit-pdos/xv6-public

The xv6 teaching OS was a useful reference for cooperative scheduler design
and per-task kernel stacks.  No xv6 source is present in this repository.

---

## Bootloader and firmware

### GRUB 2
**Licence:** GPLv2  
**URL:** https://www.gnu.org/software/grub/

Makar is booted by GRUB 2 using the Multiboot 2 protocol.  `grub-mkimage`
and `grub-mkrescue` are used in the build toolchain.  GRUB is not
distributed with Makar; it is consumed as a build tool only.

---

## Libraries and runtimes

### musl libc
**Licence:** MIT  
**URL:** https://musl.libc.org

The intended future libc for Makar userspace once `fork()`/`posix_spawn()`
and a full fd table are in place.  No musl source is present yet.

### lwIP
**Licence:** BSD 3-Clause  
**URL:** https://savannah.nongnu.org/projects/lwip/

Identified as the preferred TCP/IP stack for future Makar networking.
No lwIP source is present yet.

---

## Fonts and assets

### IBM PC VGA 8×8 glyph ROM
**Status:** Public domain  

The `FONT8x8` glyph table in `kernel/include/kernel/vesa_font.h` is based
on the classic IBM PC VGA 8×8 character ROM, widely reproduced in open-source
OS projects and considered public domain.  See `LICENSES/vesa-font.md`.

### IBM PC VGA 8×16 glyph ROM (via Linux kernel)
**Status:** Public domain (glyphs); Linux source is GPLv2  
**URL:** https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/lib/fonts/font_8x16.c

The `FONT8x16` glyph table in `kernel/include/kernel/vga_8x16_font.h` was
retyped from Linux's `lib/fonts/font_8x16.c`, which is itself a verbatim
copy of the IBM VGA BIOS character ROM (treated as public domain across
Linux, the BSDs, FreeDOS, GRUB, etc.).  No Linux source code is linked
into Makar; only the glyph data is reused.  Gratitude is documented in
`LICENSES/vga-8x16-font.md`.

The font is uploaded to VGA plane 2 on every transition out of VESA so
`setmode 80x25` / `setmode 80x50` come up readable.

---

## Documentation and learning resources

### OSDev Wiki
**Licence:** CC BY-SA  
**URL:** https://wiki.osdev.org

Invaluable reference for cross-compiler setup, paging, descriptor tables,
PIT, PS/2 keyboard, IDE PIO, VESA/VBE, and Multiboot 2.  Directly consulted
for the PMM, GDT/IDT, paging, and IDE driver implementations.

### osdev.org forums
Community Q&A that resolved many bare-metal edge cases during development.

---

## Toolchain

### GCC / Binutils (i686-elf cross-compiler)
**Licence:** GPLv3 (GCC) / GPLv2+ (Binutils)  
**URL:** https://gcc.gnu.org / https://www.gnu.org/software/binutils/

The `arawn780/gcc-cross-i686-elf:fast` Docker image ships GCC 13.2 and
Binutils 2.41 cross-compiled for the `i686-elf` bare-metal target.

### QEMU
**Licence:** GPLv2  
**URL:** https://www.qemu.org

Used for emulation during development and CI boot testing.

---

*All original Makar source code is released under the MIT License.
See [LICENSE](../LICENSE) for the full text.*
