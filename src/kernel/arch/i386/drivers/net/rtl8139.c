#include "rtl8139.h"
#include <kernel/asm.h>
#include <kernel/netdev.h>
#include <kernel/pmm.h>
#include <string.h>

#define RTL_IDR0      0x00u
#define RTL_TXSTAT0   0x10u
#define RTL_TXADDR0   0x20u
#define RTL_RXBUF     0x30u
#define RTL_CHIPCMD   0x37u
#define RTL_CAPR      0x38u
#define RTL_CBR       0x3Au
#define RTL_IMR       0x3Cu
#define RTL_ISR       0x3Eu
#define RTL_TCR       0x40u
#define RTL_RCR       0x44u
#define RTL_CONFIG1   0x52u

#define RTL_CMD_RESET 0x10u
#define RTL_CMD_RX_EN 0x08u
#define RTL_CMD_TX_EN 0x04u
#define RTL_CMD_RX_EMPTY 0x01u

#define RTL_ISR_ROK   0x0001u
#define RTL_ISR_TOK   0x0004u
#define RTL_ISR_RXERR 0x0002u
#define RTL_ISR_TXERR 0x0008u

/* Ethernet frame geometry.  ETH_MTU is the largest L3 payload; standard
 * Ethernet caps it at 1500 (no jumbo frames here).  The NIC appends the
 * 4-byte FCS itself on TX and includes it in the RX length, so the on-wire
 * maximum is header + MTU + FCS = 1518. */
#define ETH_HDR_LEN   14u
#define ETH_MTU       1500u
#define ETH_FCS_LEN   4u
#define ETH_MIN_FRAME 60u
#define ETH_MAX_FRAME (ETH_HDR_LEN + ETH_MTU)              /* 1514, FCS added by NIC */
#define ETH_MAX_WIRE  (ETH_MAX_FRAME + ETH_FCS_LEN)        /* 1518, FCS on the wire */

#define RTL_RX_RING   8192u            /* RBLEN=00 -> 8 KiB receive ring */
#define RTL_RX_BUFSZ  (RTL_RX_RING + 16u + ETH_MAX_FRAME)  /* + WRAP overhang pad */
#define RTL_TX_SLOTS  4u
#define RTL_TX_BUFSZ  2048u

/* TSD bit 13: set by the NIC when it hands the descriptor back to the host
 * (transmit complete).  Clear => the NIC still owns it, a frame is in flight. */
#define RTL_TSD_HOST_OWNS 0x2000u

typedef struct {
    int up;
    uint16_t io;
    uint8_t mac[6];
    uint32_t rx_phys;
    uint8_t *rx_buf;
    uint32_t rx_cur;
    uint32_t tx_phys;
    uint8_t *tx_buf;
    uint8_t tx_slot;
    uint8_t tx_started;   /* bitmask of TX slots submitted at least once */
} rtl8139_dev_t;

static rtl8139_dev_t s_rtl;

static int rtl_present(void) { return s_rtl.up; }
static const uint8_t *rtl_mac(void) { return s_rtl.up ? s_rtl.mac : 0; }
static int rtl_send(const void *frame, uint16_t len);
static int rtl_rx_poll(void *buf, uint16_t bufsz);

static const netdev_ops_t rtl_ops = {
    .name = "rtl8139",
    .present = rtl_present,
    .mac = rtl_mac,
    .send = rtl_send,
    .rx_poll = rtl_rx_poll,
};

static uint8_t rb(uint16_t off) { return inb((uint16_t)(s_rtl.io + off)); }
static uint32_t rl(uint16_t off) { return inl((uint16_t)(s_rtl.io + off)); }
static void wb(uint16_t off, uint8_t v) { outb((uint16_t)(s_rtl.io + off), v); }
static void ww(uint16_t off, uint16_t v) { outw((uint16_t)(s_rtl.io + off), v); }
static void wl(uint16_t off, uint32_t v) { outl((uint16_t)(s_rtl.io + off), v); }

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

static int rtl_probe(pci_device_t *dev)
{
    uint32_t io = pci_bar_io(dev, 0);
    if (!io || io > 0xFFFFu)
        return -1;

    memset(&s_rtl, 0, sizeof(s_rtl));
    s_rtl.io = (uint16_t)io;

    unsigned rx_order = order_for_bytes(RTL_RX_BUFSZ);
    unsigned tx_order = order_for_bytes(RTL_TX_SLOTS * RTL_TX_BUFSZ);
    if (rx_order >= PMM_MAX_ORDER || tx_order >= PMM_MAX_ORDER)
        return -1;
    s_rtl.rx_phys = pmm_alloc_pages(rx_order);
    s_rtl.tx_phys = pmm_alloc_pages(tx_order);
    if (s_rtl.rx_phys == PMM_ALLOC_ERROR || s_rtl.tx_phys == PMM_ALLOC_ERROR)
        return -1;
    s_rtl.rx_buf = (uint8_t *)(uintptr_t)s_rtl.rx_phys;
    s_rtl.tx_buf = (uint8_t *)(uintptr_t)s_rtl.tx_phys;
    memset(s_rtl.rx_buf, 0, (1u << rx_order) * PMM_FRAME_SIZE);
    memset(s_rtl.tx_buf, 0, (1u << tx_order) * PMM_FRAME_SIZE);

    pci_enable_bus_master(dev);
    mask_irq(dev->irq_line);

    wb(RTL_CONFIG1, 0x00);                 /* power on */
    wb(RTL_CHIPCMD, RTL_CMD_RESET);
    for (uint32_t i = 0; i < 100000 && (rb(RTL_CHIPCMD) & RTL_CMD_RESET); i++)
        ;
    if (rb(RTL_CHIPCMD) & RTL_CMD_RESET)
        return -1;

    for (uint16_t i = 0; i < 6; i++)
        s_rtl.mac[i] = rb((uint16_t)(RTL_IDR0 + i));

    wl(RTL_RXBUF, s_rtl.rx_phys);
    ww(RTL_IMR, 0x0000);
    ww(RTL_ISR, 0xFFFF);
    wl(RTL_RCR, 0x0000000Fu | (1u << 7) | (7u << 8)); /* AB/AM/APM/AAP + WRAP + unlimited DMA */
    wl(RTL_TCR, 0x03000000u);
    wb(RTL_CHIPCMD, RTL_CMD_RX_EN | RTL_CMD_TX_EN);
    s_rtl.rx_cur = 0;
    ww(RTL_CAPR, 0xFFF0u);

    s_rtl.up = 1;
    netdev_register(&rtl_ops);
    return 0;
}

static int rtl_send(const void *frame, uint16_t len)
{
    if (!s_rtl.up || !frame || len == 0 || len > ETH_MAX_FRAME)
        return -1;

    uint8_t slot = s_rtl.tx_slot & 3u;
    /* Once a slot has been used, the NIC must hand the descriptor back
     * (TxHostOwns set) before we may overwrite its buffer; otherwise the
     * previous frame is still in flight and we would corrupt it.  The first
     * use of each slot skips the check because the reset TSD has OWN clear. */
    if ((s_rtl.tx_started & (1u << slot)) &&
        !(rl((uint16_t)(RTL_TXSTAT0 + slot * 4u)) & RTL_TSD_HOST_OWNS))
        return -1;

    uint16_t send_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;
    uint8_t *dst = s_rtl.tx_buf + (uint32_t)slot * RTL_TX_BUFSZ;
    memset(dst, 0, send_len);
    memcpy(dst, frame, len);

    wl((uint16_t)(RTL_TXADDR0 + slot * 4u),
       s_rtl.tx_phys + (uint32_t)slot * RTL_TX_BUFSZ);
    wl((uint16_t)(RTL_TXSTAT0 + slot * 4u), ((uint32_t)0x3Fu << 16) | send_len);

    s_rtl.tx_started |= (uint8_t)(1u << slot);
    s_rtl.tx_slot = (uint8_t)((slot + 1u) & 3u);
    return 0;
}

static void rtl_copy_rx(void *dst, uint32_t off, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    /* RCR has the WRAP bit set, so a frame that would run past the 8 KiB ring
     * is written contiguously into the trailing pad instead of wrapping.  Read
     * linearly; rx_buf is sized RTL_RX_RING + 16 + ETH_MAX_FRAME to cover the
     * worst-case overhang, so off + len never exceeds the allocation. */
    for (uint32_t i = 0; i < len; i++)
        d[i] = s_rtl.rx_buf[off + i];
}

static int rtl_rx_poll(void *buf, uint16_t bufsz)
{
    if (!s_rtl.up || !buf)
        return -1;
    if (rb(RTL_CHIPCMD) & RTL_CMD_RX_EMPTY)
        return 0;

    uint32_t off = s_rtl.rx_cur % RTL_RX_RING;
    uint8_t hdr[4];
    rtl_copy_rx(hdr, off, sizeof(hdr));
    uint16_t status = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    uint16_t len = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
    if (!(status & 0x0001u) || len < ETH_FCS_LEN || len > ETH_MAX_WIRE) {
        ww(RTL_ISR, RTL_ISR_ROK | RTL_ISR_RXERR);
        return -1;
    }

    uint32_t frame_len = (uint32_t)len - ETH_FCS_LEN; /* strip Ethernet CRC */
    uint32_t copy_len = frame_len < bufsz ? frame_len : bufsz;
    rtl_copy_rx(buf, off + 4u, copy_len);

    /* Advance the read pointer (+4 for the rx header, round up to dword) and
     * keep it within the ring so the CAPR write below stays a valid 8 KiB
     * offset across wraps. */
    s_rtl.rx_cur = ((off + len + 4u + 3u) & ~3u) % RTL_RX_RING;
    ww(RTL_CAPR, (uint16_t)(s_rtl.rx_cur - 16u));
    ww(RTL_ISR, RTL_ISR_ROK | RTL_ISR_TOK | RTL_ISR_RXERR | RTL_ISR_TXERR);
    return (int)copy_len;
}

static const pci_driver_t rtl_driver = {
    .name = "rtl8139",
    .vendor = RTL8139_VENDOR,
    .device = RTL8139_DEVICE,
    .class_code = 0,
    .subclass = 0,
    .match_class = 0,
    .probe = rtl_probe,
};

void rtl8139_register(void)
{
    pci_register_driver(&rtl_driver);
}
