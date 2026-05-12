/*
 * elf.c - ELF32 executable loader for i386.
 *
 * elf_exec() reads an ELF32 ET_EXEC binary from the VFS, maps its PT_LOAD
 * segments into a fresh per-process page directory, and drops to ring 3 at
 * the ELF entry point.  Never returns on success.
 *
 * User programs must be linked so that all PT_LOAD segments fall above
 * 0x10000000 (the kernel 256 MiB large-page boundary).  The conventional
 * base address is 0x40000000; use a linker script such as:
 *
 *   ENTRY(_start)
 *   SECTIONS { . = 0x40000000; .text : { *(.text) } ... }
 */

#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/ring3.h>
#include <kernel/descr_tbl.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <string.h>

/* User stack top - same convention as usertest.c. */
#define ELF_STACK_TOP   0xBFFF0000u
#define PAGE_SIZE       PMM_FRAME_SIZE

/* Maximum ELF file size that the staging buffer can hold. */
#define ELF_BUF_MAX     (512u * 1024u)

/* Static staging buffer so it does not live on any task stack. */
static uint8_t s_elf_buf[ELF_BUF_MAX];

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1u); }
static inline uint32_t align_up  (uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* -------------------------------------------------------------------------
 * elf_exec
 * ------------------------------------------------------------------------- */

#define ELF_MAX_ARGC  16
#define ELF_ARG_MAX   256

int elf_exec(const char *path, int argc, const char *const *argv)
{
    /* 1. Read file into staging buffer. */
    uint32_t filesz = 0;
    if (vfs_read_file(path, s_elf_buf, ELF_BUF_MAX, &filesz) != 0) {
        t_writestring("exec: cannot read '");
        t_writestring(path);
        t_writestring("'\n");
        return -1;
    }
    if (filesz < sizeof(Elf32_Ehdr)) {
        t_writestring("exec: file too small to be ELF\n");
        return -1;
    }

    /* 2. Validate ELF header. */
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)s_elf_buf;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        t_writestring("exec: not an ELF file\n");
        return -1;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        t_writestring("exec: not a 32-bit ELF\n");
        return -1;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        t_writestring("exec: not little-endian ELF\n");
        return -1;
    }
    if (ehdr->e_type != ET_EXEC) {
        t_writestring("exec: not an executable ELF (ET_EXEC required)\n");
        return -1;
    }
    if (ehdr->e_machine != EM_386) {
        t_writestring("exec: not an i386 ELF\n");
        return -1;
    }
    if (ehdr->e_phnum == 0 || ehdr->e_phentsize < sizeof(Elf32_Phdr)) {
        t_writestring("exec: no usable program headers\n");
        return -1;
    }
    if (ehdr->e_phoff + (uint32_t)ehdr->e_phnum * ehdr->e_phentsize > filesz) {
        t_writestring("exec: program header table out of range\n");
        return -1;
    }
    if (ehdr->e_entry < 0x10000000u) {
        t_writestring("exec: entry point in kernel window (< 256 MiB)\n");
        return -1;
    }

    /* 3. Close any fds open from a previous exec and create page directory. */
    syscall_reset_fds();
    uint32_t *pd = vmm_create_pd();
    if (!pd) {
        t_writestring("exec: vmm_create_pd failed\n");
        return -1;
    }

    /* 4. Map PT_LOAD segments. */
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (s_elf_buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;

        if (ph->p_vaddr < 0x10000000u) {
            t_writestring("exec: segment below 256 MiB boundary\n");
            vmm_free_pd(pd);
            return -1;
        }
        if (ph->p_offset + ph->p_filesz > filesz) {
            t_writestring("exec: segment file data out of range\n");
            vmm_free_pd(pd);
            return -1;
        }

        uint32_t flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W)
            flags |= VMM_FLAG_WRITABLE;

        uint32_t va  = align_down(ph->p_vaddr, PAGE_SIZE);
        uint32_t end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

        for (; va < end; va += PAGE_SIZE) {
            uint32_t phys = pmm_alloc_frame();
            if (phys == PMM_ALLOC_ERROR) {
                t_writestring("exec: out of physical memory\n");
                vmm_free_pd(pd);
                return -1;
            }
            memset((void *)phys, 0, PAGE_SIZE);

            /* Copy the portion of file data that falls in this page. */
            uint32_t file_start = ph->p_vaddr;
            uint32_t file_end   = ph->p_vaddr + ph->p_filesz;
            uint32_t page_end   = va + PAGE_SIZE;

            uint32_t copy_from = (file_start > va)       ? file_start : va;
            uint32_t copy_to   = (file_end   < page_end) ? file_end   : page_end;

            if (copy_from < copy_to) {
                uint32_t dst_off = copy_from - va;
                uint32_t src_off = ph->p_offset + (copy_from - ph->p_vaddr);
                memcpy((uint8_t *)phys + dst_off,
                       s_elf_buf + src_off,
                       copy_to - copy_from);
            }

            vmm_map_page(pd, va, phys, flags);
        }
    }

    /* 5. Record the initial heap break (page-aligned end of highest segment). */
    uint32_t top_vaddr = 0;
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (s_elf_buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint32_t end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        if (end > top_vaddr) top_vaddr = end;
    }
    task_current()->user_brk = top_vaddr;

    /* 6. Map user stack (one page, read-write) and write argc/argv. */
    uint32_t stack_phys = pmm_alloc_frame();
    if (stack_phys == PMM_ALLOC_ERROR) {
        t_writestring("exec: out of physical memory (stack)\n");
        vmm_free_pd(pd);
        return -1;
    }
    memset((void *)stack_phys, 0, PAGE_SIZE);

    uint32_t stack_virt = ELF_STACK_TOP - PAGE_SIZE;   /* = 0xBFFE0000 */
    vmm_map_page(pd, stack_virt, stack_phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);

    /*
     * Build the initial user stack following the Linux i386 / ELKS / Fuzix ABI
     * so that standard crt0 code works unchanged:
     *
     *   high addr (ELF_STACK_TOP)
     *     argv strings, NUL-terminated, packed from the top
     *     [4-byte aligned gap]
     *     envp[0] = NULL            (empty environment)
     *     argv[argc] = NULL
     *     argv[argc-1]  ...  argv[0]
     *   [initial_esp]:  argc        <- _start reads this
     *   low addr (stack grows down from initial_esp)
     *
     * _start reads argc from [esp], computes argv as &[esp+4], and calls
     * main(argc, argv, envp) conventionally.
     */
    if (argc < 0) argc = 0;
    if (argc > ELF_MAX_ARGC) argc = ELF_MAX_ARGC;

    uint8_t  *spage = (uint8_t *)stack_phys;
    uint32_t  off   = PAGE_SIZE;          /* byte offset from spage[0], grows down */
    uint32_t  uargv[ELF_MAX_ARGC + 1];   /* virtual addresses of argv strings      */

    /* Pack argv strings from the top of the page downward. */
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        uint32_t len = 0;
        while (s[len] && len < (uint32_t)(ELF_ARG_MAX - 1))
            len++;
        if (off < len + 1u) {
            /* Strings overflow the page - truncate argument list. */
            argc = i;
            break;
        }
        off -= len + 1u;
        memcpy(spage + off, s, len);
        spage[off + len] = '\0';
        uargv[i] = stack_virt + off;
    }

    /* Align down to 4 bytes before writing pointer-sized values. */
    off &= ~3u;

    /*
     * Minimum space required below 'off' for the pointer table + argc:
     *   1 word  envp[0]=NULL
     *   1 word  argv[argc]=NULL
     *   argc words  argv[0..argc-1]
     *   1 word  argc
     */
    uint32_t needed = (uint32_t)(argc + 3) * 4u;
    if (off < needed) {
        /* Pathological case - give up on arguments rather than corrupt memory. */
        argc = 0;
        off  = PAGE_SIZE & ~3u;
    }

    /* envp[0] = NULL (empty environment). */
    off -= 4u; *(uint32_t *)(spage + off) = 0u;

    /* argv[argc] = NULL sentinel. */
    off -= 4u; *(uint32_t *)(spage + off) = 0u;

    /* argv[0..argc-1] pointers (argv[argc-1] written first → argv[0] at lowest). */
    for (int i = argc - 1; i >= 0; i--) {
        off -= 4u;
        *(uint32_t *)(spage + off) = uargv[i];
    }

    /* argc - _start reads this at the initial ESP. */
    off -= 4u;
    *(uint32_t *)(spage + off) = (uint32_t)argc;

    uint32_t initial_esp = stack_virt + off;

    /* 7. Activate the address space and enter ring 3. */
    task_current()->page_dir = pd;
    tss_set_kernel_stack((uint32_t)(task_current()->stack + TASK_STACK_SIZE));
    vmm_switch(pd);
    ring3_enter(ehdr->e_entry, initial_esp);   /* never returns */
}
