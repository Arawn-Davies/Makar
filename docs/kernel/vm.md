---
title: vm
parent: Kernel subsystems
grand_parent: Reference
---

# vm - hypervisor / virtual-machine detection

**Header:** `kernel/include/kernel/vm.h`
**Source:** `arch/i386/core/vm.c`

Identifies the platform Makar is running on so drivers can apply VM-specific
quirks — Hyper-V's PS/2 mouse Y convention, per-hypervisor IDE DMA policy,
framebuffer paths. Detection is best-effort, runs **once** at boot
(`vm_detect()`, after the PCI scan so it can use bus signals), and the result is
cached. This is the seam the conventions call **fallback-gating**: a quirk keys
off `vm_kind()` and the tested QEMU path is never disturbed.

---

## Detection method

- `CPUID.1:ECX[31]` is the "hypervisor present" bit.
- `CPUID` leaf `0x40000000` returns a 12-byte vendor signature identifying the
  hypervisor *interface*: `Microsoft Hv`, `VMwareVMware`, `VBoxVBoxVBox`,
  `KVMKVMKVM`, `TCGTCGTCGTCG` (QEMU/TCG), `XenVMMXenVMM`.
- **QEMU** is confirmed via its `fw_cfg` device (port `0x510/0x511` signature
  `QEMU`), distinguishing QEMU (incl. QEMU+KVM) from a bare KVM host.
- **Bochs** has no hypervisor CPUID leaf; it's identified by the Bochs/DISPI VBE
  id at port `0x1CE/0x1CF` when no other hypervisor was found.

## API

| Function | Role |
|---|---|
| `vm_detect()` | Probe the platform and cache the result. Call once, early in boot. |
| `vm_kind()` | The detected platform (`VM_BAREMETAL` until `vm_detect` runs). |
| `vm_name()` | A short lowercase name (`"qemu"`, `"hyperv"`, `"bare metal"`…) for boot logs / `/proc`. Never `NULL`. |

## `enum vm_kind`

`VM_BAREMETAL`, `VM_QEMU`, `VM_KVM`, `VM_BOCHS`, `VM_HYPERV`, `VM_VIRTUALBOX`,
`VM_VMWARE`, `VM_XEN`, `VM_UNKNOWN_HV` (hypervisor present, vendor unrecognised).

---

The detected name appears on the boot console (`Virtualization: <name>`) and in
`/proc/cpuinfo` via [procfs](procfs.md).
