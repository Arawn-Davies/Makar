/*
 * vm.c -- hypervisor / VM detection (see kernel/vm.h).
 */
#include <kernel/vm.h>
#include <kernel/asm.h>
#include <kernel/pci.h>
#include <stdint.h>
#include <string.h>

static enum vm_kind s_kind = VM_BAREMETAL;
static const char  *s_name = "bare metal";

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

/* QEMU's fw_cfg device: select the signature item (0x0000) on the selector
 * port, then read 4 bytes from the data port.  QEMU returns "QEMU"; other
 * platforms leave the port floating / unimplemented (reads !=  "QEMU"). */
static int qemu_fwcfg_present(void)
{
    outw(0x510, 0x0000);            /* FW_CFG_SIGNATURE */
    char s[4];
    for (int i = 0; i < 4; i++) s[i] = (char)inb(0x511);
    return s[0]=='Q' && s[1]=='E' && s[2]=='M' && s[3]=='U';
}

/* Bochs/QEMU VBE (DISPI) id register: index 0 at 0x1CE, value at 0x1CF.
 * A real adapter answers 0xB0C0..0xB0CF.  Used only as a Bochs tie-breaker
 * after QEMU/other hypervisors have been ruled out. */
static int bochs_vbe_present(void)
{
    outw(0x1CE, 0x0000);
    uint16_t id = inw(0x1CF);
    return id >= 0xB0C0 && id <= 0xB0CF;
}

/* True if any enumerated PCI device has the given vendor id (vm_detect runs
 * after pci_probe_all, so pci_devices[] is populated). */
static int pci_vendor_present(uint16_t vendor)
{
    for (int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].vendor_id == vendor)
            return 1;
    return 0;
}

/* PCI vendor ids used as platform tells. */
#define PCI_VENDOR_VIRTUALBOX  0x80EEu   /* InnoTek/Oracle VirtualBox VMM device */

/* Scan the hypervisor CPUID range for a 12-byte vendor signature, mirroring
 * Linux's hypervisor_cpuid_base(): a hypervisor may place its leaves at a base
 * other than 0x40000000 (e.g. when another paravirt interface occupies the base
 * leaf, or under nested virtualisation), so step across the whole range rather
 * than reading 0x40000000 alone.  `len` is the meaningful signature length
 * (12 for most, 9 for the null-padded "KVMKVMKVM").  Returns 1 on a match. */
static int hv_cpuid_sig(const char *sig, int len)
{
    for (uint32_t base = 0x40000000u; base < 0x40010000u; base += 0x100u) {
        uint32_t a, b, c, d;
        char v[12];
        cpuid(base, &a, &b, &c, &d);
        memcpy(v + 0, &b, 4); memcpy(v + 4, &c, 4); memcpy(v + 8, &d, 4);
        if (!memcmp(v, sig, (unsigned)len)) return 1;
    }
    return 0;
}

void vm_detect(void)
{
    uint32_t a, b, c, d;

    /* VirtualBox is authoritative via its VMM PCI device, checked first: VBox
     * only exposes the CPUID hypervisor leaf when a paravirt provider is
     * selected, so it is otherwise misread as Bochs (its SVGA also answers the
     * Bochs VBE id).  The VMM device is always present regardless. */
    if (pci_vendor_present(PCI_VENDOR_VIRTUALBOX)) {
        s_kind = VM_VIRTUALBOX; s_name = "virtualbox";
        return;
    }

    cpuid(1, &a, &b, &c, &d);
    int hv_present = (c >> 31) & 1;

    if (hv_present) {
        if      (hv_cpuid_sig("Microsoft Hv", 12)) { s_kind = VM_HYPERV;     s_name = "hyperv"; }
        else if (hv_cpuid_sig("VMwareVMware", 12)) { s_kind = VM_VMWARE;     s_name = "vmware"; }
        else if (hv_cpuid_sig("VBoxVBoxVBox", 12)) { s_kind = VM_VIRTUALBOX; s_name = "virtualbox"; }
        else if (hv_cpuid_sig("XenVMMXenVMM", 12)) { s_kind = VM_XEN;        s_name = "xen"; }
        else if (hv_cpuid_sig("TCGTCGTCGTCG", 12)) { s_kind = VM_QEMU;       s_name = "qemu"; }
        else if (hv_cpuid_sig("KVMKVMKVM", 9)) {
            /* KVM accelerator -- QEMU when its fw_cfg is present, else a bare
             * KVM-based VMM. */
            if (qemu_fwcfg_present()) { s_kind = VM_QEMU; s_name = "qemu"; }
            else                      { s_kind = VM_KVM;  s_name = "kvm"; }
        } else {
            s_kind = VM_UNKNOWN_HV; s_name = "hypervisor";
        }
        return;
    }

    /* No hypervisor CPUID bit: QEMU with the bit hidden, Bochs, or bare metal. */
    if (qemu_fwcfg_present())      { s_kind = VM_QEMU;  s_name = "qemu"; }
    else if (bochs_vbe_present())  { s_kind = VM_BOCHS; s_name = "bochs"; }
    else                           { s_kind = VM_BAREMETAL; s_name = "bare metal"; }
}

enum vm_kind vm_kind(void) { return s_kind; }
const char  *vm_name(void) { return s_name; }
