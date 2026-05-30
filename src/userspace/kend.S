/*
 * kend.S -- supplies the _kernel_end symbol consumed by pmm_init.
 *
 * The host kernel build gets this from arch/i386/linker.ld (`_kernel_end = .;`
 * after all sections).  The TCC link recipe used by build-kernel-tcc.sh and
 * the in-OS rebuild-kernel.sh has no GNU ld script support, so we emit the
 * symbol manually and link this object LAST -- it lands at the high end of
 * .bss and gives the same semantics.
 */
.section .bss
.global _kernel_end
_kernel_end:
.byte 0
