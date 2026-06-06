#ifndef _KERNEL_VM_H
#define _KERNEL_VM_H

/*
 * vm.h -- hypervisor / virtual-machine detection.
 *
 * Identifies the platform Makar is running on so drivers can apply
 * VM-specific quirks (e.g. Hyper-V's PS/2 mouse Y convention, per-hypervisor
 * IDE DMA policy, framebuffer paths).  Detection is best-effort and runs once
 * at boot (vm_detect); the result is cached.
 *
 * Method:
 *   - CPUID.1:ECX[31] is the "hypervisor present" bit.
 *   - CPUID leaf 0x40000000 returns a 12-byte vendor signature identifying the
 *     hypervisor *interface*: "Microsoft Hv", "VMwareVMware", "VBoxVBoxVBox",
 *     "KVMKVMKVM\0\0\0", "TCGTCGTCGTCG" (QEMU/TCG), "XenVMMXenVMM".
 *   - QEMU is confirmed via its fw_cfg device (port 0x510/0x511 signature
 *     "QEMU") -- distinguishes QEMU (incl. QEMU+KVM) from a bare KVM host.
 *   - Bochs has no hypervisor CPUID leaf; it's identified by the Bochs/DISPI
 *     VBE id at port 0x1CE/0x1CF when no other hypervisor was found.
 */

enum vm_kind {
    VM_BAREMETAL = 0,   /* no hypervisor detected                 */
    VM_QEMU,            /* QEMU (TCG or KVM-accelerated)          */
    VM_KVM,             /* KVM host, not QEMU                     */
    VM_BOCHS,           /* Bochs emulator                         */
    VM_HYPERV,          /* Microsoft Hyper-V                      */
    VM_VIRTUALBOX,      /* Oracle VirtualBox                      */
    VM_VMWARE,          /* VMware (Workstation/ESXi/Player)       */
    VM_XEN,             /* Xen                                    */
    VM_UNKNOWN_HV,      /* hypervisor present, vendor unrecognised */
};

/* Probe the platform and cache the result.  Call once, early in boot. */
void vm_detect(void);

/* The detected platform (VM_BAREMETAL until vm_detect runs). */
enum vm_kind vm_kind(void);

/* A short lowercase name for the platform ("qemu", "hyperv", "bare metal"...),
 * for boot logs / /proc.  Never NULL. */
const char *vm_name(void);

#endif /* _KERNEL_VM_H */
