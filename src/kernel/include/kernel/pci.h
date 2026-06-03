#ifndef _KERNEL_PCI_H
#define _KERNEL_PCI_H

#include <stdint.h>

/* Legacy PCI configuration space access via I/O ports. */
#define PCI_ADDR_PORT  0xCF8u
#define PCI_DATA_PORT  0xCFCu

#define PCI_MAX_DEVICES 128

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint32_t bar[6];
    const char *driver;   /* name of bound driver, NULL if unclaimed */
} pci_device_t;

extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int          pci_device_count;

/* --------------------------------------------------------------------------
 * Driver binding.  A driver matches either by vendor/device id, or (when
 * match_class is set) by class_code/subclass.  PCI_MATCH_ANY in the device
 * field matches every device of the given vendor.  pci_probe_all() walks the
 * scanned device table once and calls each registered driver's probe() on a
 * match; probe() returns 0 to claim the device (sets dev->driver).
 * -------------------------------------------------------------------------- */
#define PCI_MATCH_ANY 0xFFFFu

typedef int (*pci_probe_fn)(pci_device_t *dev);

typedef struct {
    const char  *name;
    uint16_t     vendor;       /* matched when match_class == 0 */
    uint16_t     device;       /* PCI_MATCH_ANY = any device of vendor */
    uint8_t      class_code;   /* matched when match_class != 0 */
    uint8_t      subclass;
    uint8_t      match_class;  /* 0 = match by id, 1 = match by class/subclass */
    pci_probe_fn probe;
} pci_driver_t;

void pci_register_driver(const pci_driver_t *drv);
int  pci_probe_all(void);   /* returns number of devices successfully bound */

/* Common config-space helpers used by bound drivers. */
void     pci_enable_bus_master(pci_device_t *d);
uint32_t pci_bar_io (const pci_device_t *d, int idx);  /* I/O base  (BAR & ~0x3) */
uint32_t pci_bar_mem(const pci_device_t *d, int idx);  /* MMIO base (BAR & ~0xF) */

void        pci_init(void);
uint32_t    pci_read32 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t    pci_read16 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t     pci_read8  (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void        pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);
const char *pci_vendor_name(uint16_t vendor);
const char *pci_device_name(uint16_t vendor, uint16_t device);

#endif /* _KERNEL_PCI_H */
