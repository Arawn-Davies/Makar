#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * ELF32 type definitions (System V ABI, i386 supplement).
 * ------------------------------------------------------------------------- */

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;

#define EI_NIDENT   16

/* ELF identification indices */
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5

/* Magic bytes */
#define ELFMAG0     0x7Fu
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

/* e_ident[EI_CLASS] */
#define ELFCLASS32  1

/* e_ident[EI_DATA] */
#define ELFDATA2LSB 1   /* little-endian */

/* e_type */
#define ET_EXEC     2
#define ET_DYN      3   /* shared object / PIE -- relocatable load base */

/* e_machine */
#define EM_386      3

/* p_type */
#define PT_LOAD     1
#define PT_INTERP   3   /* path to the dynamic linker (ld-musl-i386.so.1) */

/* p_flags */
#define PF_X        0x1u
#define PF_W        0x2u
#define PF_R        0x4u

typedef struct {
    uint8_t     e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word  p_type;
    Elf32_Off   p_offset;
    Elf32_Addr  p_vaddr;
    Elf32_Addr  p_paddr;
    Elf32_Word  p_filesz;
    Elf32_Word  p_memsz;
    Elf32_Word  p_flags;
    Elf32_Word  p_align;
} Elf32_Phdr;

/* -------------------------------------------------------------------------
 * Loader API
 * ------------------------------------------------------------------------- */

/*
 * elf_exec – load an ELF32 executable from the VFS and drop to ring 3.
 *
 * Reads the file at `path`, validates it as an i386 ET_EXEC ELF, maps each
 * PT_LOAD segment into a fresh VMM page directory, writes argc/argv onto the
 * user stack following the i386 cdecl ABI, and transfers control via
 * ring3_enter().  Never returns on success; returns -1 on any error.
 *
 * argc/argv are passed to the process exactly as they reach main():
 *   argv[0] is conventionally the program name (typically the path).
 *
 * All PT_LOAD segment virtual addresses must be >= 0x10000000.
 */
int elf_exec(const char *path, int argc, const char *const *argv);

/*
 * execve serialization lock.  SYS_EXECVE takes execve_lock() before filling its
 * shared argv scratch; elf_exec calls execve_unlock() once argv is packed onto
 * the new stack (so the no-return success path releases it).  Owner-based:
 * unlock is a no-op for callers that never locked (fresh-task/ktest paths).
 */
void execve_lock(void);
void execve_unlock(void);

#endif /* _KERNEL_ELF_H */
