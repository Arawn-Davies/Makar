/*
 * kend.S -- supplies the _kernel_end / _kernel_phys_end symbols consumed by
 * pmm_init (and procfs meminfo).
 *
 * The host kernel build gets these from arch/i386/linker.ld (`_kernel_end = .;`
 * and `_kernel_phys_end = . - KERNEL_VBASE;` after all sections).  The TCC link
 * recipe used by build-kernel-tcc.sh and the in-OS rebuild-kernel.sh has no GNU
 * ld script support, so we emit the symbols manually and link this object LAST
 * -- it lands at the high end of .bss and gives the same semantics.  The TCC
 * kernel is linked low-half (identity), so its physical end equals _kernel_end.
 */
.section .bss
.global _kernel_end
.global _kernel_phys_end
_kernel_end:
_kernel_phys_end:
.byte 0
