/* chainload.S – real-mode trampoline for OS chainloading
 *
 * chainload_enter(uint8_t dl):
 *   1. Copies the 16-bit trampoline blob to TRAMP_PHYS (0x7000).
 *   2. Patches the caller-supplied BIOS drive number into the blob.
 *   3. Loads a 16-bit GDT.
 *   4. Disables paging (CR0.PG = 0).
 *   5. Far-jumps via the 16-bit code descriptor into the trampoline.
 *   6. Trampoline: exits protected mode (CR0.PE = 0), enters real mode,
 *      re-enables interrupts, and hands off to the boot sector at 0x7C00.
 *
 * The caller must copy the 512-byte boot sector to physical 0x7C00 before
 * calling chainload_enter().  Never returns.
 */

/* Physical address to which we copy the 16-bit trampoline.
   Must be < 0x10000, not clobber the IVT/BDA (0x0–0x4FF),
   and not overlap the boot sector (0x7C00–0x7DFF).  0x7000 is safe. */
#define TRAMP_PHYS  0x7000

/* -----------------------------------------------------------------------
 * 16-bit GDT used during the PM → real-mode transition.
 * ----------------------------------------------------------------------- */
.section .text
.code32
.align 8
.Lgdt16:
    .quad   0x0000000000000000      /* 0x00: null                        */
    .quad   0x00CF9A000000FFFF      /* 0x08: 32-bit flat code            */
    .quad   0x00CF92000000FFFF      /* 0x10: 32-bit flat data            */
    .quad   0x00009A000000FFFF      /* 0x18: 16-bit code  base=0 lim=64K */
    .quad   0x000092000000FFFF      /* 0x20: 16-bit data  base=0 lim=64K */
.Lgdt16_end:

.Lgdt16_ptr:
    .word   .Lgdt16_end - .Lgdt16 - 1
    .long   .Lgdt16

/* -----------------------------------------------------------------------
 * 16-bit trampoline blob.
 *
 * This code is assembled at its link address inside the kernel image, but
 * chainload_enter() copies it byte-for-byte to TRAMP_PHYS before jumping
 * to it.  Every absolute address embedded here (far-jump target, DL patch
 * address) is pre-computed relative to TRAMP_PHYS so the blob is correct
 * at its runtime location.
 * ----------------------------------------------------------------------- */
.global chainload_tramp_start
chainload_tramp_start:
.code16

    /* ---- 16-bit protected mode ---------------------------------------- */
    movw    $0x20, %ax              /* load 16-bit data descriptor          */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* Clear CR0.PE - exit protected mode.
     * GAS adds the 0x66 operand-size prefix automatically in .code16. */
    movl    %cr0, %eax
    andl    $0xFFFFFFFE, %eax
    movl    %eax, %cr0

    /* Far jump to flush the pipeline and re-enter real mode.
     * The target CS:IP is 0x0000 : (TRAMP_PHYS + offset of .Lreal_mode).
     * We emit raw bytes because the linker does not know the runtime address. */
    .byte   0xEA                    /* opcode: far jmp ptr16:16             */
    .word   TRAMP_PHYS + (.Lreal_mode - chainload_tramp_start)
    .word   0x0000

    /* ---- Real mode ------------------------------------------------------ */
.Lreal_mode:
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movw    $0x7C00, %sp            /* stack top just below the boot sector */

    /* BIOS drive number - the immediate byte is patched by chainload_enter(). */
.global chainload_tramp_dl_patch
chainload_tramp_dl_patch:
    movb    $0x80, %dl              /* patched at runtime: byte 1 = drive   */

    sti                             /* BIOS interrupt handlers need IRQs     */

    /* Hand off to the boot sector at 0x0000:0x7C00. */
    ljmp    $0x0000, $0x7C00

.global chainload_tramp_end
chainload_tramp_end:

/* -----------------------------------------------------------------------
 * chainload_enter(uint8_t dl)
 *
 * Caller convention (cdecl):
 *   [esp+4]  uint8_t dl - BIOS drive number to pass to the boot sector
 *                         (0x80 = first HDD, 0x00 = first floppy, …).
 *
 * The 512-byte boot sector must already be at physical 0x7C00.
 * ----------------------------------------------------------------------- */
.code32
.global chainload_enter
chainload_enter:
    movb    4(%esp), %dl            /* save drive number in DL              */
    cli

    /* Copy the trampoline blob to TRAMP_PHYS. */
    pushl   %esi
    pushl   %edi
    pushl   %ecx
    movl    $chainload_tramp_start, %esi
    movl    $TRAMP_PHYS,            %edi
    movl    $chainload_tramp_end,   %ecx
    subl    $chainload_tramp_start, %ecx    /* byte count                   */
    rep     movsb
    popl    %ecx
    popl    %edi
    popl    %esi

    /* Patch the drive number into the copied blob.
     * chainload_tramp_dl_patch labels a 2-byte "movb $0x80, %dl" (B2 imm8).
     * The immediate is at offset +1 from the label.
     * DL is still the drive number: the copy loop only touched ESI/EDI/ECX. */
    movl    $chainload_tramp_dl_patch, %eax
    subl    $chainload_tramp_start,    %eax     /* offset within blob        */
    addl    $TRAMP_PHYS,               %eax     /* address in copied version */
    movb    %dl, 1(%eax)                        /* patch the immediate byte  */

    /* Load the 16-bit GDT. */
    lgdt    .Lgdt16_ptr

    /* Disable paging: clear CR0.PG. */
    movl    %cr0, %eax
    andl    $0x7FFFFFFF, %eax
    movl    %eax, %cr0

    /* Far jump into the 16-bit code descriptor (0x18) at TRAMP_PHYS.
     * After this the CPU executes 16-bit instructions at physical 0x7000. */
    ljmp    $0x18, $TRAMP_PHYS
