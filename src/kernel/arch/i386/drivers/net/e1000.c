#include <kernel/asm.h>
#include <kernel/netdev.h>
#include <kernel/paging.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <string.h>

#define E1000_VENDOR 0x8086u
#define E1000_DEV_82540EM 0x100Eu
#define E1000_DEV_82545EM 0x100Fu

#define E_REG_CTRL   0x0000u
#define E_REG_STATUS 0x0008u
#define E_REG_ICR    0x00C0u
#define E_REG_IMC    0x00D8u
#define E_REG_RCTL   0x0100u
#define E_REG_TCTL   0x0400u
#define E_REG_TIPG   0x0410u
#define E_REG_RDBAL  0x2800u
#define E_REG_RDLEN  0x2808u
#define E_REG_RDH    0x2810u
#define E_REG_RDT    0x2818u
#define E_REG_TDBAL  0x3800u
#define E_REG_TDLEN  0x3808u
#define E_REG_TDH    0x3810u
#define E_REG_TDT    0x3818u
#define E_REG_RAL    0x5400u
#define E_REG_RAH    0x5404u
#define E_REG_MTA    0x5200u

#define E_CTRL_RST   (1u << 26)
#define E_RCTL_EN    (1u << 1)
#define E_RCTL_BAM   (1u << 15)
#define E_RCTL_SECRC (1u << 26)
#define E_RAH_AV     (1u << 31)
#define E_TCTL_EN    (1u << 1)
#define E_TCTL_PSP   (1u << 3)

#define E_TX_CMD_EOP 0x01u
#define E_TX_CMD_IFCS 0x02u
#define E_TX_CMD_RS  0x08u
#define E_DESC_DD    0x01u

#define E_RX_COUNT 16u
#define E_TX_COUNT 8u
#define E_BUF_SIZE 2048u
#define E_MMIO_SIZE 0x20000u

/* Ethernet frame geometry: standard MTU, no jumbo frames.  The NIC appends
 * the FCS, so the largest frame we hand it is header + MTU = 1514. */
#define ETH_HDR_LEN   14u
#define ETH_MTU       1500u
#define ETH_MIN_FRAME 60u
#define ETH_MAX_FRAME (ETH_HDR_LEN + ETH_MTU)

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e_tx_desc_t;

typedef struct {
    int up;
    uint16_t io;
    volatile uint8_t *mmio;
    uint8_t mac[6];
    uint32_t rx_desc_phys;
    e_rx_desc_t *rx_desc;
    uint32_t tx_desc_phys;
    e_tx_desc_t *tx_desc;
    uint32_t rx_buf_phys;
    uint8_t *rx_buf;
    uint32_t tx_buf_phys;
    uint8_t *tx_buf;
    uint32_t rx_next;
    uint32_t tx_next;
} e1000_dev_t;

static e1000_dev_t s_e;

static int e_present(void) { return s_e.up; }
static const uint8_t *e_mac(void) { return s_e.up ? s_e.mac : 0; }
static int e_send(const void *frame, uint16_t len);
static int e_rx_poll(void *buf, uint16_t bufsz);

static const netdev_ops_t e_ops = {
    .name = "e1000",
    .present = e_present,
    .mac = e_mac,
    .send = e_send,
    .rx_poll = e_rx_poll,
};

static void ew(uint32_t reg, uint32_t val)
{
    if (s_e.mmio) {
        *(volatile uint32_t *)(s_e.mmio + reg) = val;
    } else {
        outl(s_e.io, reg);
        outl((uint16_t)(s_e.io + 4), val);
    }
}

static uint32_t er(uint32_t reg)
{
    if (s_e.mmio)
        return *(volatile uint32_t *)(s_e.mmio + reg);
    outl(s_e.io, reg);
    return inl((uint16_t)(s_e.io + 4));
}

static void mask_irq(uint8_t irq)
{
    if (irq == 0 || irq >= 16) return;
    uint16_t port = (irq < 8) ? 0x21u : 0xA1u;
    uint8_t bit = (irq < 8) ? irq : (uint8_t)(irq - 8);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

static unsigned order_for_bytes(uint32_t bytes)
{
    uint32_t pages = (bytes + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    unsigned order = 0;
    uint32_t have = 1;
    while (have < pages && order + 1 < PMM_MAX_ORDER) {
        have <<= 1;
        order++;
    }
    return have >= pages ? order : PMM_MAX_ORDER;
}

static void e_delay(void)
{
    for (volatile uint32_t i = 0; i < 100000; i++)
        ;
}

static int mac_empty_or_bcast(const uint8_t mac[6])
{
    uint8_t all_zero = 1;
    uint8_t all_ff = 1;
    for (uint32_t i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = 0;
        if (mac[i] != 0xFF) all_ff = 0;
    }
    return all_zero || all_ff;
}

static void e_set_mac(const uint8_t mac[6])
{
    uint32_t ral = (uint32_t)mac[0]
                 | ((uint32_t)mac[1] << 8)
                 | ((uint32_t)mac[2] << 16)
                 | ((uint32_t)mac[3] << 24);
    uint32_t rah = (uint32_t)mac[4]
                 | ((uint32_t)mac[5] << 8)
                 | E_RAH_AV;
    ew(E_REG_RAL, ral);
    ew(E_REG_RAH, rah);
}

static int e_alloc(void)
{
    unsigned rdo = order_for_bytes(sizeof(e_rx_desc_t) * E_RX_COUNT);
    unsigned tdo = order_for_bytes(sizeof(e_tx_desc_t) * E_TX_COUNT);
    unsigned rbo = order_for_bytes(E_RX_COUNT * E_BUF_SIZE);
    unsigned tbo = order_for_bytes(E_TX_COUNT * E_BUF_SIZE);
    if (rdo >= PMM_MAX_ORDER || tdo >= PMM_MAX_ORDER ||
        rbo >= PMM_MAX_ORDER || tbo >= PMM_MAX_ORDER)
        return -1;

    s_e.rx_desc_phys = pmm_alloc_pages(rdo);
    s_e.tx_desc_phys = pmm_alloc_pages(tdo);
    s_e.rx_buf_phys = pmm_alloc_pages(rbo);
    s_e.tx_buf_phys = pmm_alloc_pages(tbo);
    if (s_e.rx_desc_phys == PMM_ALLOC_ERROR || s_e.tx_desc_phys == PMM_ALLOC_ERROR ||
        s_e.rx_buf_phys == PMM_ALLOC_ERROR || s_e.tx_buf_phys == PMM_ALLOC_ERROR)
        return -1;

    s_e.rx_desc = (e_rx_desc_t *)(uintptr_t)s_e.rx_desc_phys;
    s_e.tx_desc = (e_tx_desc_t *)(uintptr_t)s_e.tx_desc_phys;
    s_e.rx_buf = (uint8_t *)(uintptr_t)s_e.rx_buf_phys;
    s_e.tx_buf = (uint8_t *)(uintptr_t)s_e.tx_buf_phys;
    memset(s_e.rx_desc, 0, (1u << rdo) * PMM_FRAME_SIZE);
    memset(s_e.tx_desc, 0, (1u << tdo) * PMM_FRAME_SIZE);
    memset(s_e.rx_buf, 0, (1u << rbo) * PMM_FRAME_SIZE);
    memset(s_e.tx_buf, 0, (1u << tbo) * PMM_FRAME_SIZE);
    return 0;
}

static int e_probe(pci_device_t *dev)
{
    uint32_t mem = pci_bar_mem(dev, 0);
    uint32_t io = pci_bar_io(dev, 1);
    if (!mem && (!io || io > 0xFFFFu))
        return -1;

    memset(&s_e, 0, sizeof(s_e));
    if (mem) {
        paging_map_region(mem, E_MMIO_SIZE);
        s_e.mmio = (volatile uint8_t *)(uintptr_t)mem;
    } else {
        s_e.io = (uint16_t)io;
    }
    if (e_alloc() != 0)
        return -1;

    pci_enable_bus_master(dev);
    mask_irq(dev->irq_line);

    ew(E_REG_CTRL, er(E_REG_CTRL) | E_CTRL_RST);
    e_delay();
    ew(E_REG_IMC, 0xFFFFFFFFu);
    (void)er(E_REG_ICR);
    for (uint32_t i = 0; i < 128; i++)
        ew(E_REG_MTA + i * 4u, 0);

    uint32_t ral = er(E_REG_RAL);
    uint32_t rah = er(E_REG_RAH);
    s_e.mac[0] = (uint8_t)(ral & 0xFF);
    s_e.mac[1] = (uint8_t)((ral >> 8) & 0xFF);
    s_e.mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    s_e.mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    s_e.mac[4] = (uint8_t)(rah & 0xFF);
    s_e.mac[5] = (uint8_t)((rah >> 8) & 0xFF);
    if (mac_empty_or_bcast(s_e.mac)) {
        static const uint8_t qemu_default_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
        memcpy(s_e.mac, qemu_default_mac, sizeof(s_e.mac));
    }
    e_set_mac(s_e.mac);

    ew(E_REG_RCTL, 0);
    ew(E_REG_TCTL, 0);

    for (uint32_t i = 0; i < E_RX_COUNT; i++) {
        s_e.rx_desc[i].addr = s_e.rx_buf_phys + i * E_BUF_SIZE;
        s_e.rx_desc[i].status = 0;
    }
    for (uint32_t i = 0; i < E_TX_COUNT; i++)
        s_e.tx_desc[i].status = E_DESC_DD;

    ew(E_REG_RDBAL, s_e.rx_desc_phys);
    ew(E_REG_RDBAL + 4u, 0);
    ew(E_REG_RDLEN, sizeof(e_rx_desc_t) * E_RX_COUNT);
    ew(E_REG_RDH, 0);
    ew(E_REG_RDT, E_RX_COUNT - 1u);

    ew(E_REG_TDBAL, s_e.tx_desc_phys);
    ew(E_REG_TDBAL + 4u, 0);
    ew(E_REG_TDLEN, sizeof(e_tx_desc_t) * E_TX_COUNT);
    ew(E_REG_TDH, 0);
    ew(E_REG_TDT, 0);
    ew(E_REG_TIPG, 0x0060200Au);

    ew(E_REG_RCTL, E_RCTL_EN | E_RCTL_BAM | E_RCTL_SECRC);
    ew(E_REG_TCTL, E_TCTL_EN | E_TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    (void)er(E_REG_STATUS);

    s_e.up = 1;
    netdev_register(&e_ops);
    return 0;
}

static int e_send(const void *frame, uint16_t len)
{
    if (!s_e.up || !frame || len == 0 || len > ETH_MAX_FRAME)
        return -1;
    uint32_t idx = s_e.tx_next % E_TX_COUNT;
    if (!(s_e.tx_desc[idx].status & E_DESC_DD))
        return -1;
    uint16_t send_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;
    uint8_t *dst = s_e.tx_buf + idx * E_BUF_SIZE;
    memset(dst, 0, send_len);
    memcpy(dst, frame, len);

    s_e.tx_desc[idx].addr = s_e.tx_buf_phys + idx * E_BUF_SIZE;
    s_e.tx_desc[idx].length = send_len;
    s_e.tx_desc[idx].cmd = E_TX_CMD_EOP | E_TX_CMD_IFCS | E_TX_CMD_RS;
    s_e.tx_desc[idx].status = 0;
    s_e.tx_next = (idx + 1u) % E_TX_COUNT;
    ew(E_REG_TDT, s_e.tx_next);
    return 0;
}

static int e_rx_poll(void *buf, uint16_t bufsz)
{
    if (!s_e.up || !buf)
        return -1;
    uint32_t idx = s_e.rx_next % E_RX_COUNT;
    e_rx_desc_t *d = &s_e.rx_desc[idx];
    if (!(d->status & E_DESC_DD))
        return 0;

    uint32_t len = d->length;
    uint32_t copy_len = len < bufsz ? len : bufsz;
    memcpy(buf, s_e.rx_buf + idx * E_BUF_SIZE, copy_len);
    d->status = 0;
    s_e.rx_next = (idx + 1u) % E_RX_COUNT;
    ew(E_REG_RDT, idx);
    return (int)copy_len;
}

static const pci_driver_t e_driver_82540 = {
    .name = "e1000",
    .vendor = E1000_VENDOR,
    .device = E1000_DEV_82540EM,
    .probe = e_probe,
};

static const pci_driver_t e_driver_82545 = {
    .name = "e1000",
    .vendor = E1000_VENDOR,
    .device = E1000_DEV_82545EM,
    .probe = e_probe,
};

void e1000_register(void)
{
    pci_register_driver(&e_driver_82540);
    pci_register_driver(&e_driver_82545);
}
