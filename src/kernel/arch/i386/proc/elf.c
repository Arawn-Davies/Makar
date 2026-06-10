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
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/ring3.h>
#include <kernel/descr_tbl.h>
#include <kernel/task.h>
#include <kernel/syscall.h>
#include <kernel/tty.h>
#include <kernel/timer.h>
#include <string.h>

/* User stack top - same convention as usertest.c. */
#define ELF_STACK_TOP   0xBFFF0000u
#define PAGE_SIZE       PMM_FRAME_SIZE

/* Number of 4 KiB pages mapped for the initial user stack.  TCC's
 * recursive-descent parser plus its symbol/type/expression stacks blow
 * past a single 4 KiB page when compiling non-trivial inputs (e.g.
 * sh.c).  32 KiB is well clear of that and still cheap; auto-grow can
 * come later if a workload outgrows this.  Stack lives at
 * [ELF_STACK_TOP - USER_STACK_PAGES*PAGE_SIZE, ELF_STACK_TOP). */
#define USER_STACK_PAGES  8u

/* Maximum ELF file size that the staging buffer can hold.  Sized for the
 * largest image we load whole: musl's libc.so / ld-musl (~820 KiB) and tcc.elf
 * (~926 KiB), with headroom. */
#define ELF_BUF_MAX     (2u * 1024u * 1024u)

/* Static staging buffer so it does not live on any task stack.  Reused for the
 * dynamic interpreter after the main image's segments are copied into frames. */
static uint8_t s_elf_buf[ELF_BUF_MAX];

/* Dynamic-linking layout (user space < 0xC0000000):
 *   PIE main image base        DYN_BASE     (ET_DYN; ET_EXEC keeps its own vaddrs)
 *   dynamic interpreter base   INTERP_BASE  (ld-musl-i386.so.1, itself ET_DYN)
 * The interpreter then mmaps libc.so into the 0x90000000 anon/mmap window.
 * All clear of each other and the 0xBFFF0000 stack. */
#define DYN_BASE        0x50000000u
#define INTERP_BASE     0x70000000u

/* System V i386 auxv entry types musl reads (a_type values). */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_RANDOM  25
#define AT_EXECFN  31

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1u); }
static inline uint32_t align_up  (uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* -------------------------------------------------------------------------
 * execve serialization lock.
 *
 * SYS_EXECVE copies the caller's argv into a single large file-scope scratch
 * buffer (its user PD is about to be freed), which elf_exec then packs onto the
 * new task's stack.  Under preemptible syscalls two execves would clobber that
 * scratch, so SYS_EXECVE takes this lock on entry; elf_exec releases it the
 * moment argv has been consumed onto the new stack (execve_unlock below), just
 * before the no-return ring3_enter -- so the success path frees it too.  Owner-
 * based, so the fresh-task / ktest callers of elf_exec (which don't take it)
 * unlock as a no-op.  Yields while held by another task; irq-guarded test/set.
 * ------------------------------------------------------------------------- */
static inline uint32_t elf_irq_save(void)
{ uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void elf_irq_restore(uint32_t f)
{ __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }

static volatile int s_execve_owner = -1;
void execve_lock(void)
{
    task_t *t = task_current();
    int me = t ? t->pid : -2;
    for (;;) {
        uint32_t fl = elf_irq_save();
        if (s_execve_owner == -1) { s_execve_owner = me; elf_irq_restore(fl); return; }
        elf_irq_restore(fl);
        task_yield();
    }
}
void execve_unlock(void)
{
    task_t *t = task_current();
    int me = t ? t->pid : -2;
    uint32_t fl = elf_irq_save();
    if (s_execve_owner == me) s_execve_owner = -1;   /* no-op if we never held it */
    elf_irq_restore(fl);
}

/* -------------------------------------------------------------------------
 * elf_exec
 * ------------------------------------------------------------------------- */

#define ELF_MAX_ARGC  128   /* room for tcc.elf's full kernel-rebuild link line */
#define ELF_ARG_MAX   256

/* Map an ELF image's PT_LOAD segments into `pd` at `base + p_vaddr`, copying
 * file data from `buf` and zero-filling the rest (bss).  `base` is 0 for an
 * ET_EXEC, the chosen load base for an ET_DYN (PIE) or the interpreter.  Each
 * segment keeps its own R/W permission (i386 has no NX, so X is implicit).
 * Returns 0, or -1 (the caller frees the page directory). */
static int map_load_segments(uint32_t *pd, const uint8_t *buf, uint32_t filesz,
                             const Elf32_Ehdr *ehdr, uint32_t base)
{
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;

        uint32_t vaddr = ph->p_vaddr + base;
        if (vaddr < 0x10000000u)                  return -1;  /* into kernel window */
        if (ph->p_offset + ph->p_filesz > filesz) return -1;

        uint32_t flags = VMM_FLAG_USER;
        if (ph->p_flags & PF_W) flags |= VMM_FLAG_WRITABLE;

        uint32_t va  = align_down(vaddr, PAGE_SIZE);
        uint32_t end = align_up(vaddr + ph->p_memsz, PAGE_SIZE);

        for (; va < end; va += PAGE_SIZE) {
            uint32_t phys = pmm_alloc_frame();
            if (phys == PMM_ALLOC_ERROR) return -1;
            memset((void *)phys, 0, PAGE_SIZE);

            uint32_t file_start = vaddr;
            uint32_t file_end   = vaddr + ph->p_filesz;
            uint32_t page_end   = va + PAGE_SIZE;
            uint32_t copy_from  = (file_start > va)       ? file_start : va;
            uint32_t copy_to    = (file_end   < page_end) ? file_end   : page_end;

            if (copy_from < copy_to) {
                uint32_t dst_off = copy_from - va;
                uint32_t src_off = ph->p_offset + (copy_from - vaddr);
                memcpy((uint8_t *)phys + dst_off, buf + src_off, copy_to - copy_from);
            }
            vmm_map_page(pd, va, phys, flags);
        }
    }
    return 0;
}

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
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        t_writestring("exec: not an executable ELF (ET_EXEC / ET_DYN)\n");
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
    /* ET_EXEC entries are absolute and must clear the low identity window.
     * ET_DYN (PIE) entries are relative -- load_base (0x50000000+) is added
     * below -- so they're legitimately small here; don't reject them. */
    if (ehdr->e_type == ET_EXEC && ehdr->e_entry < 0x10000000u) {
        t_writestring("exec: entry point in kernel window (< 256 MiB)\n");
        return -1;
    }

    /* 3. Create the user page directory. (Per-task fd tables make a global
     * reset unnecessary - the child task owns its own table from
     * task_create and it's reaped when the task dies.) */
    uint32_t *pd = vmm_create_pd();
    if (!pd) {
        t_writestring("exec: vmm_create_pd failed\n");
        return -1;
    }

    /* 4. Load base: ET_DYN (PIE) is relocatable; ET_EXEC keeps its vaddrs.
     *    Map the main image's PT_LOAD segments. */
    uint32_t load_base = (ehdr->e_type == ET_DYN) ? DYN_BASE : 0u;
    if (map_load_segments(pd, s_elf_buf, filesz, ehdr, load_base) != 0) {
        t_writestring("exec: bad or oversized PT_LOAD segment\n");
        vmm_free_pd(pd);
        return -1;
    }

    /* Save what the auxv needs from the main ehdr BEFORE the staging buffer is
     * reused for the interpreter (the main's segments are now in frames, so
     * s_elf_buf is free). */
    uint32_t entry_main = ehdr->e_entry      + load_base;
    uint32_t at_phent   = ehdr->e_phentsize;
    uint32_t at_phnum   = ehdr->e_phnum;

    /* AT_PHDR is the *in-memory* address of the program headers.  The phdrs live
     * at file offset e_phoff; find the PT_LOAD that maps that offset and
     * translate to a vaddr: load_base + p_vaddr + (e_phoff - p_offset).  Using
     * load_base + e_phoff is wrong for ET_EXEC (segment vaddr is 0x40000000, not
     * 0) and only happens to work for PIE because its first segment vaddr is 0;
     * ld.so/__libc_start_main deref AT_PHDR, so a bad value faults near NULL. */
    uint32_t at_phdr = ehdr->e_phoff + load_base;   /* fallback */
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (s_elf_buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);
        if (ph->p_type == PT_LOAD &&
            ehdr->e_phoff >= ph->p_offset &&
            ehdr->e_phoff <  ph->p_offset + ph->p_filesz) {
            at_phdr = load_base + ph->p_vaddr + (ehdr->e_phoff - ph->p_offset);
            break;
        }
    }

    /* 5. Heap break = end of the highest main segment (+ base). */
    uint32_t top_vaddr = 0;
    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (s_elf_buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        uint32_t end = align_up(ph->p_vaddr + load_base + ph->p_memsz, PAGE_SIZE);
        if (end > top_vaddr) top_vaddr = end;
    }

    /* 5b. Find PT_INTERP and copy the interpreter path out before reuse.
     * Only an ET_DYN (PIE) image drives the dynamic linker; an ET_EXEC may
     * still carry a vestigial PT_INTERP (TCC stamps /lib/ld-linux.so.2 on its
     * otherwise self-contained output), which we ignore exactly as the
     * pre-dynamic loader did -- ET_EXEC stays jump-straight-to-entry. */
    char interp_path[128];
    int  has_interp = 0;
    for (int i = 0; ehdr->e_type == ET_DYN && i < (int)ehdr->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)
            (s_elf_buf + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);
        if (ph->p_type != PT_INTERP) continue;
        uint32_t n = ph->p_filesz;
        if (n == 0 || n > sizeof interp_path)   break;
        if (ph->p_offset + n > filesz)          break;
        memcpy(interp_path, s_elf_buf + ph->p_offset, n);
        interp_path[n - 1] = '\0';      /* p_filesz includes the NUL */
        has_interp = 1;
        break;
    }

    task_current()->user_brk = top_vaddr;
    task_current()->user_brk_base = top_vaddr;  /* lower bound of the lazy brk region */
    task_current()->mmap_next = 0;   /* fresh address space: reset anon-mmap window */
    task_current()->tls_gs = 0x23u;  /* fresh image: drop any prior TLS */
    task_current()->tls_active = 0;

    /* 5c. Dynamic executable: load the interpreter (ld-musl-i386.so.1, itself
     * ET_DYN) at INTERP_BASE and enter *there* instead of the program entry.
     * musl's ld.so then mmaps libc.so (DT_NEEDED), relocates, and jumps to
     * AT_ENTRY.  Reuses s_elf_buf (the main image is already in frames). */
    uint32_t enter_entry = entry_main;
    uint32_t at_base     = 0;
    if (has_interp) {
        uint32_t isz = 0;
        if (vfs_read_file(interp_path, s_elf_buf, ELF_BUF_MAX, &isz) != 0 ||
            isz < sizeof(Elf32_Ehdr)) {
            t_writestring("exec: cannot read interpreter '");
            t_writestring(interp_path); t_writestring("'\n");
            vmm_free_pd(pd); return -1;
        }
        const Elf32_Ehdr *ie = (const Elf32_Ehdr *)s_elf_buf;
        if (ie->e_ident[EI_MAG0] != ELFMAG0 || ie->e_machine != EM_386 ||
            ie->e_type != ET_DYN) {
            t_writestring("exec: bad interpreter ELF\n");
            vmm_free_pd(pd); return -1;
        }
        if (map_load_segments(pd, s_elf_buf, isz, ie, INTERP_BASE) != 0) {
            t_writestring("exec: interpreter segment out of range\n");
            vmm_free_pd(pd); return -1;
        }
        enter_entry = ie->e_entry + INTERP_BASE;
        at_base     = INTERP_BASE;
    }

    /* 6. Map user stack (USER_STACK_PAGES pages, read-write).  Only the
     * top page is used to write argc/argv; the rest grow downward as
     * the program runs.  Map all pages eagerly so a deep call chain
     * (e.g. TCC parsing sh.c) doesn't fault past the bottom. */
    uint32_t stack_phys_top = 0;          /* physical of the highest page */
    for (uint32_t i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (phys == PMM_ALLOC_ERROR) {
            t_writestring("exec: out of physical memory (stack)\n");
            vmm_free_pd(pd);
            return -1;
        }
        memset((void *)phys, 0, PAGE_SIZE);
        uint32_t va = ELF_STACK_TOP - (i + 1) * PAGE_SIZE;
        vmm_map_page(pd, va, phys, VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        if (i == 0) stack_phys_top = phys;
    }
    uint32_t stack_phys = stack_phys_top;
    uint32_t stack_virt = ELF_STACK_TOP - PAGE_SIZE;   /* top page = 0xBFFEF000 */

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

    /* 16 bytes of entropy for AT_RANDOM (musl reads it for the stack/TLS
     * canary).  Packed in the string area above the pointer table. */
    uint32_t rand_ptr = 0;
    if (off >= 16u) {
        off -= 16u;
        uint32_t seed = timer_get_ticks() ^ 0x9E3779B9u;
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245u + 12345u;
            spage[off + i] = (uint8_t)(seed >> 16);
        }
        rand_ptr = stack_virt + off;
    }

    /* Align down to 4 bytes before writing pointer-sized values. */
    off &= ~3u;

    /*
     * Minimum space below 'off' for the pointer table + argc:
     *   18 words  auxv: PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/RANDOM/EXECFN/NULL
     *   1 word    envp[0]=NULL
     *   1 word    argv[argc]=NULL
     *   argc words  argv[0..argc-1]
     *   1 word    argc
     */
    uint32_t needed = (uint32_t)(argc + 21) * 4u;
    if (off < needed) {
        /* Pathological case - give up on arguments rather than corrupt memory. */
        argc = 0;
        off  = PAGE_SIZE & ~3u;
    }

    /* Full System V i386 auxv, highest addr -> lowest (AT_NULL terminates);
     * each entry is {a_type, a_val} with a_type at the lower address.  The
     * dynamic linker reads AT_PHDR/PHENT/PHNUM (the program's own headers),
     * AT_BASE (where ld.so itself was loaded) and AT_ENTRY (the program entry).
     * Static musl only needs AT_RANDOM + AT_PAGESZ.  AT_EXECFN = argv[0]. */
    uint32_t execfn = (argc > 0) ? uargv[0] : 0u;
    off -= 4u; *(uint32_t *)(spage + off) = 0u;          /* AT_NULL    val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_NULL;     /* AT_NULL    type */
    off -= 4u; *(uint32_t *)(spage + off) = execfn;      /* AT_EXECFN  val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_EXECFN;   /* AT_EXECFN  type */
    off -= 4u; *(uint32_t *)(spage + off) = rand_ptr;    /* AT_RANDOM  val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_RANDOM;   /* AT_RANDOM  type */
    off -= 4u; *(uint32_t *)(spage + off) = entry_main;  /* AT_ENTRY   val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_ENTRY;    /* AT_ENTRY   type */
    off -= 4u; *(uint32_t *)(spage + off) = at_base;     /* AT_BASE    val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_BASE;     /* AT_BASE    type */
    off -= 4u; *(uint32_t *)(spage + off) = PAGE_SIZE;   /* AT_PAGESZ  val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_PAGESZ;   /* AT_PAGESZ  type */
    off -= 4u; *(uint32_t *)(spage + off) = at_phnum;    /* AT_PHNUM   val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_PHNUM;    /* AT_PHNUM   type */
    off -= 4u; *(uint32_t *)(spage + off) = at_phent;    /* AT_PHENT   val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_PHENT;    /* AT_PHENT   type */
    off -= 4u; *(uint32_t *)(spage + off) = at_phdr;     /* AT_PHDR    val  */
    off -= 4u; *(uint32_t *)(spage + off) = AT_PHDR;     /* AT_PHDR    type */

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

    /* argv has now been fully packed onto the new task's stack; the caller's
     * execve scratch is free to reuse, so drop the execve lock before the
     * no-return ring3 entry (no-op for non-SYS_EXECVE callers). */
    execve_unlock();

    /* 7. Activate the address space and enter ring 3.
     *
     * For execve from an existing user task, the calling task's page_dir
     * is already a user PD that we must free after switching CR3 -- if
     * we don't, fork+exec leaks every PD the child ever ran under.  For
     * the fresh-task path (exec_task_entry) page_dir is paging_kernel_pd()
     * and must NOT be freed.  The selector below handles both cases. */
    task_t *cur = task_current();
    uint32_t *old_pd = cur->page_dir;
    cur->page_dir = pd;
    tss_set_kernel_stack((uint32_t)(cur->stack + TASK_STACK_SIZE));
    vmm_switch(pd);
    if (old_pd && old_pd != paging_kernel_pd())
        vmm_free_pd(old_pd);
    ring3_enter(enter_entry, initial_esp);   /* interp (dynamic) or program entry */
}
