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
} pci_device_t;

extern pci_device_t pci_devices[PCI_MAX_DEVICES];
extern int          pci_device_count;

void        pci_init(void);
uint32_t    pci_read32 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t    pci_read16 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t     pci_read8  (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void        pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif /* _KERNEL_PCI_H */
