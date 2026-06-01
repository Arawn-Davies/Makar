#include <kernel/pci.h>
#include <kernel/asm.h>
#include <kernel/serial.h>

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

/* --------------------------------------------------------------------------
 * Vendor / device name table.
 * Source: https://pci-ids.ucw.cz/  (fetched 2026-06-01, curated subset)
 * device_id == 0xFFFF is a vendor-only sentinel (no device match).
 * -------------------------------------------------------------------------- */
typedef struct { uint16_t vendor; uint16_t device; const char *vendor_name; const char *device_name; } pci_id_entry_t;

static const pci_id_entry_t pci_id_table[] = {
    /* QEMU / bochs (0x1234 not in pci-ids DB, but widely known) */
    { 0x1234, 0x1111, "QEMU",                                 "Standard VGA" },
    /* Intel */
    { 0x8086, 0x1237, "Intel Corporation",                    "440FX - 82441FX PMC [Natoma]" },
    { 0x8086, 0x7000, "Intel Corporation",                    "82371SB PIIX3 ISA [Natoma/Triton II]" },
    { 0x8086, 0x7010, "Intel Corporation",                    "82371SB PIIX3 IDE [Natoma/Triton II]" },
    { 0x8086, 0x7020, "Intel Corporation",                    "82371SB PIIX3 USB [Natoma/Triton II]" },
    { 0x8086, 0x7111, "Intel Corporation",                    "82371AB/EB/MB PIIX4 IDE" },
    { 0x8086, 0x100E, "Intel Corporation",                    "82540EM Gigabit Ethernet Controller" },
    { 0x8086, 0x100F, "Intel Corporation",                    "82545EM Gigabit Ethernet Controller (Copper)" },
    { 0x8086, 0x10D3, "Intel Corporation",                    "82574L Gigabit Network Connection" },
    { 0x8086, 0x1502, "Intel Corporation",                    "82579LM Gigabit Network Connection" },
    { 0x8086, 0x1503, "Intel Corporation",                    "82579V Gigabit Network Connection" },
    { 0x8086, 0x2415, "Intel Corporation",                    "82801AA AC'97 Audio Controller" },
    { 0x8086, 0x2445, "Intel Corporation",                    "82801BA/BAM AC'97 Audio Controller" },
    { 0x8086, 0x2668, "Intel Corporation",                    "ICH6 High Definition Audio Controller" },
    { 0x8086, 0x2922, "Intel Corporation",                    "ICH9R 6-port SATA Controller [AHCI]" },
    { 0x8086, 0x2934, "Intel Corporation",                    "ICH9 USB UHCI Controller" },
    { 0x8086, 0x3A38, "Intel Corporation",                    "ICH10 USB UHCI Controller" },
    /* Realtek */
    { 0x10EC, 0x8029, "Realtek Semiconductor",                "RTL-8029(AS) 10/100 Ethernet" },
    { 0x10EC, 0x8139, "Realtek Semiconductor",                "RTL-8139 PCI Fast Ethernet" },
    { 0x10EC, 0x8168, "Realtek Semiconductor",                "RTL8111/8168 Gigabit Ethernet" },
    { 0x10EC, 0x8169, "Realtek Semiconductor",                "RTL8169 Gigabit Ethernet" },
    { 0x10EC, 0x8125, "Realtek Semiconductor",                "RTL8125 2.5GbE Controller" },
    { 0x10EC, 0x5229, "Realtek Semiconductor",                "RTS5229 PCIe Card Reader" },
    { 0x10EC, 0x5250, "Realtek Semiconductor",                "RTS5250 PCIe Card Reader" },
    /* Red Hat virtio */
    { 0x1AF4, 0x1000, "Red Hat",                              "Virtio network device" },
    { 0x1AF4, 0x1001, "Red Hat",                              "Virtio block device" },
    { 0x1AF4, 0x1002, "Red Hat",                              "Virtio memory balloon" },
    { 0x1AF4, 0x1003, "Red Hat",                              "Virtio console" },
    { 0x1AF4, 0x1004, "Red Hat",                              "Virtio SCSI" },
    { 0x1AF4, 0x1005, "Red Hat",                              "Virtio RNG" },
    { 0x1AF4, 0x1009, "Red Hat",                              "Virtio filesystem" },
    { 0x1AF4, 0x1041, "Red Hat",                              "Virtio 1.0 network device" },
    { 0x1AF4, 0x1042, "Red Hat",                              "Virtio 1.0 block device" },
    { 0x1AF4, 0x1050, "Red Hat",                              "Virtio 1.0 GPU" },
    { 0x1AF4, 0x1052, "Red Hat",                              "Virtio 1.0 input" },
    { 0x1AF4, 0x1110, "Red Hat",                              "QEMU Inter-VM shared memory" },
    /* QEMU devices (Red Hat 0x1B36) */
    { 0x1B36, 0x0001, "QEMU",                                 "PCI-PCI bridge" },
    { 0x1B36, 0x0002, "QEMU",                                 "PCI 16550A Adapter" },
    { 0x1B36, 0x0008, "QEMU",                                 "PCIe Host bridge" },
    { 0x1B36, 0x000C, "QEMU",                                 "PCIe Root port" },
    { 0x1B36, 0x000D, "QEMU",                                 "xHCI Host Controller" },
    { 0x1B36, 0x0010, "QEMU",                                 "NVM Express Controller" },
    { 0x1B36, 0x0100, "QEMU",                                 "QXL paravirtual GPU" },
    /* VMware */
    { 0x15AD, 0x0405, "VMware",                               "SVGA II Adapter" },
    { 0x15AD, 0x0770, "VMware",                               "USB2 EHCI Controller" },
    { 0x15AD, 0x0774, "VMware",                               "USB1.1 UHCI Controller" },
    { 0x15AD, 0x07B0, "VMware",                               "VMXNET3 Ethernet Controller" },
    { 0x15AD, 0x07C0, "VMware",                               "PVSCSI Controller" },
    /* VirtualBox */
    { 0x80EE, 0xBEEF, "VirtualBox",                           "Graphics Adapter" },
    { 0x80EE, 0xCAFE, "VirtualBox",                           "Guest Service" },
    /* AMD */
    { 0x1022, 0x2000, "AMD",                                  "PCnet32 LANCE" },
    { 0x1022, 0x7801, "AMD",                                  "FCH SATA Controller [AHCI]" },
    { 0x1022, 0x7808, "AMD",                                  "FCH USB EHCI Controller" },
    /* AMD/ATI */
    { 0x1002, 0x4752, "AMD/ATI",                              "Rage XL PCI" },
    /* Broadcom */
    { 0x14E4, 0x1644, "Broadcom",                             "NetXtreme BCM5700 Gigabit Ethernet" },
    { 0x14E4, 0x1645, "Broadcom",                             "NetXtreme BCM5701 Gigabit Ethernet" },
    { 0x14E4, 0x165A, "Broadcom",                             "NetXtreme BCM5722 Gigabit Ethernet PCIe" },
};
#define PCI_ID_TABLE_SIZE ((int)(sizeof(pci_id_table)/sizeof(pci_id_table[0])))

const char *pci_vendor_name(uint16_t vendor)
{
    for (int i = 0; i < PCI_ID_TABLE_SIZE; i++)
        if (pci_id_table[i].vendor == vendor)
            return pci_id_table[i].vendor_name;
    return NULL;
}

const char *pci_device_name(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < PCI_ID_TABLE_SIZE; i++)
        if (pci_id_table[i].vendor == vendor && pci_id_table[i].device == device)
            return pci_id_table[i].device_name;
    return NULL;
}

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
