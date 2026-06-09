---
title: pci
parent: Kernel subsystems
grand_parent: Reference
---

# pci - PCI bus scan + driver binding

**Header:** `kernel/include/kernel/pci.h`
**Source:** `arch/i386/drivers/pci.c`

Legacy PCI configuration-space access via I/O ports (`PCI_ADDR_PORT 0xCF8` /
`PCI_DATA_PORT 0xCFC`). `pci_init()` enumerates the bus into a flat table
(`pci_devices[]`, up to `PCI_MAX_DEVICES = 128`); `pci_probe_all()` then walks it
once and offers each device to the registered drivers. This is the bind point
for the NICs ([networking](../networking.md)), the SVGA II display backend
([video](video.md)), and bus-master IDE DMA ([ide](ide.md)).

---

## Driver model

A `pci_driver_t` matches **either** by vendor/device id (`match_class = 0`,
`device = PCI_MATCH_ANY` to match any device of a vendor) **or** by
class/subclass (`match_class = 1`). `pci_probe_all()` calls each matched
driver's `probe(dev)`; returning `0` **claims** the device (sets `dev->driver`).

```c
void pci_register_driver(const pci_driver_t *drv);   /* register before pci_probe_all */
int  pci_probe_all(void);                            /* returns devices bound */
```

Boot order: drivers register (`ide_pci_register`, `virtio_net_register`,
`rtl8139_register`, `e1000_register`, `pcnet_register`), then `pci_init()` scans,
then `pci_probe_all()` binds.

## Config + BAR helpers (for bound drivers)

| Function | Role |
|---|---|
| `pci_read8/16/32`, `pci_write32(bus,dev,func,off,…)` | Raw config-space access. |
| `pci_enable_bus_master(d)` | Set the bus-master bit (DMA-capable devices). |
| `pci_bar_io(d,idx)` | I/O base (`BAR & ~0x3`). |
| `pci_bar_mem(d,idx)` | MMIO base (`BAR & ~0xF`). |
| `pci_class_name` / `pci_vendor_name` / `pci_device_name` | Human names for `lspci` / boot logs. |

## `pci_device_t`

`bus/dev/func`, `vendor_id`, `device_id`, `class_code`, `subclass`, `prog_if`,
`revision_id`, `header_type`, `irq_line`, `bar[6]`, and `driver` (the bound
driver's name, `NULL` if unclaimed). The userspace `lspci` tool formats this
table.
