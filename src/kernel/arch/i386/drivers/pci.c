#include <kernel/pci.h>
#include <kernel/asm.h>
#include <kernel/serial.h>

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

/* --------------------------------------------------------------------------
 * Config-space access via legacy I/O ports 0xCF8 / 0xCFC.
 * PCIe extended config space (MCFG MMIO) will be added when ACPI MCFG
 * parsing is implemented; these helpers cover all devices for now.
 * -------------------------------------------------------------------------- */

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (1u << 31)
         | ((uint32_t)bus           << 16)
         | ((uint32_t)(dev  & 0x1F) << 11)
         | ((uint32_t)(func &  0x7) <<  8)
         | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_ADDR_PORT, pci_addr(bus, dev, func, off));
    return inl(PCI_DATA_PORT);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint16_t)(pci_read32(bus, dev, func, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return (uint8_t)(pci_read32(bus, dev, func, off) >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    outl(PCI_ADDR_PORT, pci_addr(bus, dev, func, off));
    outl(PCI_DATA_PORT, val);
}

/* --------------------------------------------------------------------------
 * Class/subclass name table (enough to label QEMU's device list usefully).
 * -------------------------------------------------------------------------- */

const char *pci_class_name(uint8_t cls, uint8_t sub)
{
    switch (cls) {
    case 0x00: return sub == 0x01 ? "VGA compat"  : "Unclassified";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI";
        case 0x01: return "IDE";
        case 0x05: return "ATA";
        case 0x06: return "SATA (AHCI)";
        case 0x08: return "NVMe";
        default:   return "Storage";
        }
    case 0x02:
        switch (sub) {
        case 0x00: return "Ethernet";
        case 0x80: return "Network (other)";
        default:   return "Network";
        }
    case 0x03: return "Display";
    case 0x04:
        switch (sub) {
        case 0x01: return "Audio";
        default:   return "Multimedia";
        }
    case 0x06:
        switch (sub) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-PCI Bridge";
        default:   return "Bridge";
        }
    case 0x0C:
        switch (sub) {
        case 0x00: return "FireWire";
        case 0x03: return "USB";
        case 0x05: return "SMBus";
        default:   return "Serial Bus";
        }
    default: return "Unknown";
    }
}

/* --------------------------------------------------------------------------
 * Enumeration
 * -------------------------------------------------------------------------- */

static void scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = pci_read32(bus, dev, func, 0);
    if ((id & 0xFFFF) == 0xFFFF) return;
    if (pci_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *d = &pci_devices[pci_device_count++];
    d->bus  = bus;
    d->dev  = dev;
    d->func = func;
    d->vendor_id = (uint16_t)(id & 0xFFFF);
    d->device_id = (uint16_t)(id >> 16);

    uint32_t cr = pci_read32(bus, dev, func, 0x08);
    d->revision_id = (uint8_t) cr;
    d->prog_if     = (uint8_t)(cr >>  8);
    d->subclass    = (uint8_t)(cr >> 16);
    d->class_code  = (uint8_t)(cr >> 24);

    d->header_type = pci_read8(bus, dev, func, 0x0E);

    if ((d->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_read32(bus, dev, func, (uint8_t)(0x10 + i * 4));
        d->irq_line = pci_read8(bus, dev, func, 0x3C);
    } else {
        for (int i = 0; i < 6; i++) d->bar[i] = 0;
        d->irq_line = 0;
    }
}

static void scan_device(uint8_t bus, uint8_t dev)
{
    if ((pci_read32(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF) return;
    scan_function(bus, dev, 0);
    if (pci_read8(bus, dev, 0, 0x0E) & 0x80) {
        for (uint8_t f = 1; f < 8; f++) {
            if ((pci_read32(bus, dev, f, 0) & 0xFFFF) != 0xFFFF)
                scan_function(bus, dev, f);
        }
    }
}

void pci_init(void)
{
    pci_device_count = 0;
    for (uint32_t bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            scan_device((uint8_t)bus, dev);

    Serial_WriteString("pci: ");
    Serial_WriteDec((uint32_t)pci_device_count);
    Serial_WriteString(" device(s) found\n");
}
