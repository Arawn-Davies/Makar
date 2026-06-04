#include <kernel/asm.h>
#include <kernel/netdev.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <string.h>

#define PCNET_VENDOR 0x1022u
#define PCNET_DEVICE 0x2000u

#define PCNET_APROM 0x00u
#define PCNET_RDP   0x10u
#define PCNET_RAP   0x12u
#define PCNET_RESET 0x14u
#define PCNET_BDP   0x16u

#define PCNET_RX_COUNT 16u
#define PCNET_TX_COUNT 8u
#define PCNET_BUF_SIZE 1544u

/* Ethernet frame geometry: standard MTU, no jumbo frames.  The NIC appends
 * the FCS, so the largest frame we hand it is header + MTU = 1514, which fits
 * the 1544-byte descriptor buffers. */
#define ETH_HDR_LEN   14u
#define ETH_MTU       1500u
#define ETH_MIN_FRAME 60u
#define ETH_MAX_FRAME (ETH_HDR_LEN + ETH_MTU)

#define PCNET_DESC_OWN 0x8000u
#define PCNET_DESC_ERR 0x4000u
#define PCNET_DESC_STP 0x0200u
#define PCNET_DESC_ENP 0x0100u

typedef struct __attribute__((packed, aligned(16))) {
    uint16_t mode;
    uint8_t rlen;
    uint8_t tlen;
    uint8_t padr[6];
    uint16_t reserved;
    uint8_t ladrf[8];
    uint32_t rdra;
    uint32_t tdra;
} pcnet_init_t;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t addr;
    int16_t bcnt;
    volatile uint16_t status;
    uint32_t misc;
    uint32_t reserved;
} pcnet_desc_t;

typedef struct {
    int up;
    uint16_t io;
    uint8_t mac[6];
    uint32_t init_phys;
    pcnet_init_t *init;
    uint32_t rx_desc_phys;
    pcnet_desc_t *rx_desc;
    uint32_t tx_desc_phys;
    pcnet_desc_t *tx_desc;
    uint32_t rx_buf_phys;
    uint8_t *rx_buf;
    uint32_t tx_buf_phys;
    uint8_t *tx_buf;
    uint32_t rx_next;
    uint32_t tx_next;
} pcnet_dev_t;

static pcnet_dev_t s_pc;

static int pc_present(void) { return s_pc.up; }
static const uint8_t *pc_mac(void) { return s_pc.up ? s_pc.mac : 0; }
static int pc_send(const void *frame, uint16_t len);
static int pc_rx_poll(void *buf, uint16_t bufsz);

static const netdev_ops_t pc_ops = {
    .name = "pcnet",
    .present = pc_present,
    .mac = pc_mac,
    .send = pc_send,
    .rx_poll = pc_rx_poll,
};

static void csr(uint16_t reg, uint16_t val)
{
    outw((uint16_t)(s_pc.io + PCNET_RAP), reg);
    outw((uint16_t)(s_pc.io + PCNET_RDP), val);
}

static uint16_t csr_r(uint16_t reg)
{
    outw((uint16_t)(s_pc.io + PCNET_RAP), reg);
    return inw((uint16_t)(s_pc.io + PCNET_RDP));
}

static void bcr(uint16_t reg, uint16_t val)
{
    outw((uint16_t)(s_pc.io + PCNET_RAP), reg);
    outw((uint16_t)(s_pc.io + PCNET_BDP), val);
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

static int pc_alloc(void)
{
    unsigned io = order_for_bytes(sizeof(pcnet_init_t));
    unsigned rdo = order_for_bytes(sizeof(pcnet_desc_t) * PCNET_RX_COUNT);
    unsigned tdo = order_for_bytes(sizeof(pcnet_desc_t) * PCNET_TX_COUNT);
    unsigned rbo = order_for_bytes(PCNET_RX_COUNT * PCNET_BUF_SIZE);
    unsigned tbo = order_for_bytes(PCNET_TX_COUNT * PCNET_BUF_SIZE);
    if (io >= PMM_MAX_ORDER || rdo >= PMM_MAX_ORDER || tdo >= PMM_MAX_ORDER ||
        rbo >= PMM_MAX_ORDER || tbo >= PMM_MAX_ORDER)
        return -1;
    s_pc.init_phys = pmm_alloc_pages(io);
    s_pc.rx_desc_phys = pmm_alloc_pages(rdo);
    s_pc.tx_desc_phys = pmm_alloc_pages(tdo);
    s_pc.rx_buf_phys = pmm_alloc_pages(rbo);
    s_pc.tx_buf_phys = pmm_alloc_pages(tbo);
    if (s_pc.init_phys == PMM_ALLOC_ERROR || s_pc.rx_desc_phys == PMM_ALLOC_ERROR ||
        s_pc.tx_desc_phys == PMM_ALLOC_ERROR || s_pc.rx_buf_phys == PMM_ALLOC_ERROR ||
        s_pc.tx_buf_phys == PMM_ALLOC_ERROR)
        return -1;
    s_pc.init = (pcnet_init_t *)(uintptr_t)s_pc.init_phys;
    s_pc.rx_desc = (pcnet_desc_t *)(uintptr_t)s_pc.rx_desc_phys;
    s_pc.tx_desc = (pcnet_desc_t *)(uintptr_t)s_pc.tx_desc_phys;
    s_pc.rx_buf = (uint8_t *)(uintptr_t)s_pc.rx_buf_phys;
    s_pc.tx_buf = (uint8_t *)(uintptr_t)s_pc.tx_buf_phys;
    memset(s_pc.init, 0, (1u << io) * PMM_FRAME_SIZE);
    memset(s_pc.rx_desc, 0, (1u << rdo) * PMM_FRAME_SIZE);
    memset(s_pc.tx_desc, 0, (1u << tdo) * PMM_FRAME_SIZE);
    memset(s_pc.rx_buf, 0, (1u << rbo) * PMM_FRAME_SIZE);
    memset(s_pc.tx_buf, 0, (1u << tbo) * PMM_FRAME_SIZE);
    return 0;
}

static int pc_probe(pci_device_t *dev)
{
    uint32_t io = pci_bar_io(dev, 0);
    if (!io || io > 0xFFFFu)
        return -1;
    memset(&s_pc, 0, sizeof(s_pc));
    s_pc.io = (uint16_t)io;
    if (pc_alloc() != 0)
        return -1;

    pci_enable_bus_master(dev);
    mask_irq(dev->irq_line);
    (void)inw((uint16_t)(s_pc.io + PCNET_RESET));
    for (uint32_t i = 0; i < 10000; i++)
        ;
    bcr(20, 2);                         /* 32-bit software style */

    for (uint16_t i = 0; i < 6; i++) {
        s_pc.mac[i] = inb((uint16_t)(s_pc.io + PCNET_APROM + i));
        s_pc.init->padr[i] = s_pc.mac[i];
    }
    s_pc.init->mode = 0;
    s_pc.init->rlen = 4u << 4;           /* log2(16) in high nibble */
    s_pc.init->tlen = 3u << 4;           /* log2(8) in high nibble */
    s_pc.init->rdra = s_pc.rx_desc_phys;
    s_pc.init->tdra = s_pc.tx_desc_phys;

    for (uint32_t i = 0; i < PCNET_RX_COUNT; i++) {
        s_pc.rx_desc[i].addr = s_pc.rx_buf_phys + i * PCNET_BUF_SIZE;
        s_pc.rx_desc[i].bcnt = (int16_t)(-((int16_t)PCNET_BUF_SIZE));
        s_pc.rx_desc[i].status = PCNET_DESC_OWN;
    }
    for (uint32_t i = 0; i < PCNET_TX_COUNT; i++) {
        s_pc.tx_desc[i].addr = s_pc.tx_buf_phys + i * PCNET_BUF_SIZE;
        s_pc.tx_desc[i].bcnt = 0;
        s_pc.tx_desc[i].status = 0;
    }

    csr(1, (uint16_t)(s_pc.init_phys & 0xFFFFu));
    csr(2, (uint16_t)(s_pc.init_phys >> 16));
    csr(3, 0x0100u);                    /* disable interrupts */
    csr(4, 0x0915u);                    /* disable TX/RX poll interrupts */
    csr(0, 0x0001u);                    /* INIT */
    for (uint32_t i = 0; i < 100000; i++) {
        if (csr_r(0) & 0x0100u)          /* IDON */
            break;
    }
    if (!(csr_r(0) & 0x0100u))
        return -1;
    csr(0, 0x0002u);                    /* STRT */

    s_pc.up = 1;
    netdev_register(&pc_ops);
    return 0;
}

static int pc_send(const void *frame, uint16_t len)
{
    if (!s_pc.up || !frame || len == 0 || len > ETH_MAX_FRAME)
        return -1;
    uint32_t idx = s_pc.tx_next % PCNET_TX_COUNT;
    if (s_pc.tx_desc[idx].status & PCNET_DESC_OWN)
        return -1;
    uint16_t send_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;
    uint8_t *dst = s_pc.tx_buf + idx * PCNET_BUF_SIZE;
    memset(dst, 0, send_len);
    memcpy(dst, frame, len);
    s_pc.tx_desc[idx].misc = 0;
    s_pc.tx_desc[idx].bcnt = (int16_t)(-((int16_t)send_len));
    s_pc.tx_desc[idx].status = PCNET_DESC_OWN | PCNET_DESC_STP | PCNET_DESC_ENP;
    s_pc.tx_next = (idx + 1u) % PCNET_TX_COUNT;
    csr(0, 0x0008u);                    /* TDMD */
    return 0;
}

static int pc_rx_poll(void *buf, uint16_t bufsz)
{
    if (!s_pc.up || !buf)
        return -1;
    uint32_t idx = s_pc.rx_next % PCNET_RX_COUNT;
    pcnet_desc_t *d = &s_pc.rx_desc[idx];
    uint16_t st = d->status;
    if (st & PCNET_DESC_OWN)
        return 0;
    if ((st & (PCNET_DESC_ERR | PCNET_DESC_STP | PCNET_DESC_ENP)) !=
        (PCNET_DESC_STP | PCNET_DESC_ENP)) {
        d->bcnt = (int16_t)(-((int16_t)PCNET_BUF_SIZE));
        d->status = PCNET_DESC_OWN;
        s_pc.rx_next = (idx + 1u) % PCNET_RX_COUNT;
        return -1;
    }
    uint32_t len = d->misc & 0x0FFFu;
    if (len >= 4)
        len -= 4;                       /* strip Ethernet CRC */
    uint32_t copy_len = len < bufsz ? len : bufsz;
    memcpy(buf, s_pc.rx_buf + idx * PCNET_BUF_SIZE, copy_len);
    d->misc = 0;
    d->bcnt = (int16_t)(-((int16_t)PCNET_BUF_SIZE));
    d->status = PCNET_DESC_OWN;
    s_pc.rx_next = (idx + 1u) % PCNET_RX_COUNT;
    return (int)copy_len;
}

static const pci_driver_t pc_driver = {
    .name = "pcnet",
    .vendor = PCNET_VENDOR,
    .device = PCNET_DEVICE,
    .probe = pc_probe,
};

void pcnet_register(void)
{
    pci_register_driver(&pc_driver);
}
