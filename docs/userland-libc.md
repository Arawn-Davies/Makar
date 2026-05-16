# Userland libc for Makar

This document covers how to build a freestanding libc for Makar userspace and
link it against ELF binaries compiled with the i686-elf cross-compiler.  It
draws heavily from the OSDev wiki, ELKS, FUZIX, and musl's own porting notes.

---

## Background and acknowledgements

Makar's userspace design is informed by the work of several FOSS projects:

- **Linux kernel** (GPLv2) - syscall ABI conventions, ELF loading model,
  and process/memory layout.  Linux i386 syscall numbers are used directly
  (SYS_EXIT=1, SYS_READ=3, SYS_WRITE=4, etc.).
- **ELKS** (GPLv2, https://github.com/ghaerr/elks) - lightweight kernel for
  8086/286 that demonstrates how far you can push a minimal libc on real iron.
  ELKS's approach to a tiny `crt0.S` + static libc is the direct model for
  Makar's `src/userspace/` layout.
- **FUZIX** (GPLv2, https://github.com/EtchedPixels/FUZIX) - Alan Cox's
  Unix-like OS for small systems.  FUZIX's vi implementation and its approach
  to portable libc stubs across wildly different hardware influenced VIX.
- **musl libc** (MIT, https://musl.libc.org) - the preferred libc target for
  Makar once the fd table and fork() are in place.  Clean, auditable, and does
  not pull in glibc's dynamic-linker complexity.
- **CP/M** - the philosophical ancestor: a small OS that got out of the way,
  let the shell own the terminal, and expected programs to be self-contained.

All FOSS code used or referenced is attributed in the relevant source files.
Makar itself is released under the BSD-3 Clause Clear license.

---

## Current state

Makar already compiles and links a minimal freestanding libc (`src/libc/`)
into `libk.a`.  User ELF binaries currently link against `libk.a` via
`src/userspace/link.ld` + `crt0.S`.

### What is already present (as of May 2026)

| Syscall | Number | Status |
|---|---|---|
| `SYS_EXIT` | 1 | ✅ |
| `SYS_READ(fd, buf, len)` | 3 | ✅ fd 0 = keyboard (line-buffered), fd 3+ = open files |
| `SYS_WRITE(fd, buf, len)` | 4 | ✅ fd 1/2 = VGA terminal; file write NYI |
| `SYS_OPEN(path, flags)` | 5 | ✅ reads whole file into heap buffer (max 64 KiB) |
| `SYS_CLOSE(fd)` | 6 | ✅ |
| `SYS_LSEEK(fd, off, whence)` | 19 | ✅ |
| `SYS_BRK(addr)` | 45 | ✅ maps pages via VMM on demand |
| `SYS_YIELD` | 158 | ✅ |
| `SYS_DELETE_FILE(path)` | 208 | ✅ deletes FAT32 file |
| `SYS_RENAME_FILE(old, new)` | 209 | ✅ renames/moves FAT32 file or directory |
| `SYS_DELETE_DIR(path)` | 210 | ✅ deletes empty FAT32 directory |

What is **not** yet present:

| Missing piece | Needed for |
|---|---|
| `SYS_WRITE(fd, buf, len)` to open files | Full file-write from userspace |
| `SYS_GETCWD` | `getcwd()` in libc |
| `SYS_READDIR` | `opendir()` / `readdir()` |
| `fork()` / `posix_spawn()` | Multi-process apps and a userland shell |

---

## Step-by-step path to a linked libc

### 1. Fix `SYS_WRITE` to use `(fd, buf, len)`

Current `SYS_WRITE` (EAX=4) reads a NUL-terminated string from EBX.
Change `syscall.c` to the standard Linux i386 convention:

```c
// EAX=4, EBX=fd, ECX=buf ptr, EDX=len
case 4:
    if (regs->ebx == 1 || regs->ebx == 2) {  /* stdout / stderr */
        char *p = (char *)regs->ecx;
        for (uint32_t i = 0; i < regs->edx; i++)
            t_putchar(p[i]);
        regs->eax = regs->edx;
    } else {
        regs->eax = (uint32_t)-1;  /* EBADF until fd table exists */
    }
    break;
```

Update `crt0.S` and `src/libc/stdio/` write stubs to use the new convention.

### 2. Add `SYS_BRK` (heap extension)

```c
// EAX=45, EBX=new_brk (0 = query current)
```

Each user task needs a `brk` pointer stored in its `task_t`.  The VMM already
supports `vmm_map_page()`; `SYS_BRK` just maps new pages on demand.

### 3. Add the fd table

A minimal fd table lives in `task_t`:

```c
#define TASK_MAX_FDS  16
typedef struct { vfs_node_t *node; uint32_t offset; } fd_entry_t;
fd_entry_t fds[TASK_MAX_FDS];
```

fd 0 = keyboard (read-only), fd 1 = terminal (write-only), fd 2 = serial
(write-only).  `SYS_OPEN` allocates the next free slot and wraps a VFS node.

### 4. Port musl libc (preferred)

Once `SYS_WRITE(fd,buf,len)`, `SYS_BRK`, `SYS_OPEN/CLOSE/READ`, and
`SYS_GETCWD` are in place:

```sh
# In the Docker build container (arawn780/gcc-cross-i686-elf:fast):
git clone https://git.musl-libc.org/cgit/musl
cd musl
./configure \
    --target=i686-elf \
    --prefix=/usr/i686-elf \
    --syslibdir=/usr/i686-elf/lib \
    CROSS_COMPILE=i686-elf- \
    CFLAGS="-ffreestanding -nostdlib"
make && make install
```

musl does not require `fork()` for static linking.  A no-fork static binary
works as long as it never calls `system()` or `popen()`.

**Blocker:** musl's `stdio` assumes `write(1, buf, n)` works.  Once step 1
is done, basic `printf` will function.

### 5. Alternative: uClibc-ng (lighter, no fork required)

uClibc-ng targets embedded systems and is easier to configure for a no-MMU /
minimal-syscall environment:

```sh
git clone https://cgit.uclibc-ng.org/cgi/cgit/uclibc-ng.git
# Configure for i386 static, no threads, no dynamic linker
make menuconfig   # Target: i386, static only, minimal features
make CROSS=i686-elf-
```

uClibc-ng needs the same syscall set as musl but has fewer hidden
dependencies on POSIX process semantics.

### 6. Cross-compiler sysroot layout

The OSDev wiki (https://wiki.osdev.org/Porting_GCC_to_your_OS) recommends:

```
sysroot/
  usr/
    include/    ← kernel public headers + libc headers
    lib/
      libc.a    ← static libc
      libk.a    ← kernel-linked subset (already exists)
      crt0.o
      crtbegin.o / crtend.o  (from libgcc)
```

`build.sh` already uses `SYSROOT` for the kernel.  Extend it:

```sh
# Install libc headers into sysroot
make -C src/libc install     # copies headers → sysroot/usr/include
# Install musl/uClibc static archive
cp libmusl.a sysroot/usr/lib/libc.a
```

Userspace link command becomes:

```
i686-elf-gcc -ffreestanding -nostartfiles \
    -T src/userspace/link.ld \
    sysroot/usr/lib/crt0.o \
    my_app.c \
    -L sysroot/usr/lib -lc -lgcc \
    -o my_app.elf
```

### 7. In-kernel compilation (long-term goal)

Once musl or uClibc-ng is linked, the goal is to build simple C programs
**on a running Makar system** using a stripped-down `tcc` (Tiny C Compiler):

- `tcc` is ~200 KiB, requires only `malloc`/`free`/`open`/`read`/`write`
- No need for `fork()` - `tcc` compiles to an ELF in memory and writes it
- Makar's VFS write support (`vfs_write_file`) is already in place
- The ELF can then be `exec`'d directly from the shell

This mirrors the CP/M / ELKS model: boot, write code, compile, run - all on
the bare metal.

---

## Roadmap dependency graph

```
SYS_WRITE(fd,buf,len)
    └── SYS_BRK
            └── fd table (SYS_OPEN/CLOSE/READ)
                    └── SYS_GETCWD + SYS_READDIR
                            └── musl / uClibc-ng static link
                                    ├── userland shell (dash/ash)
                                    ├── tcc in-kernel compiler
                                    └── fork() + posix_spawn
                                                └── full process model
```

---

## References

- OSDev wiki - Porting GCC: https://wiki.osdev.org/Porting_GCC_to_your_OS
- OSDev wiki - Creating a libc: https://wiki.osdev.org/Creating_a_C_Library
- musl libc porting: https://musl.libc.org/doc/musl-cross-tools.html
- ELKS libc: https://github.com/ghaerr/elks/tree/master/libc
- FUZIX libc: https://github.com/EtchedPixels/FUZIX/tree/master/Library/libs
- Linux i386 syscall table: https://syscalls.w3challs.com/?arch=x86
