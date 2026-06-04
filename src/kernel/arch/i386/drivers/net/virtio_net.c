#include <kernel/virtio_net.h>
#include <kernel/netdev.h>
#include <kernel/pci.h>
#include <kernel/pmm.h>
#include <kernel/asm.h>
#include <string.h>

#define VIRTIO_PCI_VENDOR       0x1AF4u
#define VIRTIO_PCI_NET_DEVICE   0x1000u

#define VIRTIO_PCI_HOST_FEATURES  0x00u
#define VIRTIO_PCI_GUEST_FEATURES 0x04u
#define VIRTIO_PCI_QUEUE_PFN      0x08u
#define VIRTIO_PCI_QUEUE_NUM      0x0Cu
#define VIRTIO_PCI_QUEUE_SEL      0x0Eu
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10u
#define VIRTIO_PCI_STATUS         0x12u
#define VIRTIO_PCI_CONFIG         0x14u

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER      0x02u
#define VIRTIO_STATUS_DRIVER_OK   0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_STATUS_FAILED      0x80u

#define VIRTIO_NET_F_MAC          (1u << 5)

#define VIRTQ_DESC_F_NEXT         1u
#define VIRTQ_DESC_F_WRITE        2u

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1u

#define VIRTIO_NET_Q_RX           0u
#define VIRTIO_NET_Q_TX           1u

#define VIRTIO_NET_MAX_QSZ        256u
#define VIRTIO_NET_RX_BUF_SIZE    2048u
#define VIRTIO_NET_TX_BUF_SIZE    2048u
#define VIRTIO_NET_DMA_MIN_PHYS  0x00100000u

typedef struct __attribute__((packed)) {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_MAX_QSZ];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_NET_MAX_QSZ];
} virtq_used_t;

typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

typedef struct {
    uint16_t qsel;
    uint16_t size;
    uint32_t phys;
    uint32_t bytes;
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    uint16_t last_used;
} virtio_queue_t;

typedef struct {
    int up;
    uint16_t io;
    uint8_t mac[6];
    virtio_queue_t rxq;
    virtio_queue_t txq;
    uint16_t rx_slots;
    uint32_t rx_buf_phys;
    uint8_t *rx_bufs;
    uint32_t tx_buf_phys;
    uint8_t *tx_buf;
} virtio_net_dev_t;

static virtio_net_dev_t s_vnet;

static const netdev_ops_t virtio_net_ops = {
    .name = "virtio-net",
    .present = virtio_net_present,
    .mac = virtio_net_mac,
    .send = virtio_net_send,
    .rx_poll = virtio_net_rx_poll,
};

static inline void vq_barrier(void)
{
    asm volatile("" ::: "memory");
}

static inline uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static uint8_t vp_inb(uint16_t off)   { return inb((uint16_t)(s_vnet.io + off)); }
static uint16_t vp_inw(uint16_t off)  { return inw((uint16_t)(s_vnet.io + off)); }
static uint32_t vp_inl(uint16_t off)  { return inl((uint16_t)(s_vnet.io + off)); }
static void vp_outb(uint16_t off, uint8_t v)   { outb((uint16_t)(s_vnet.io + off), v); }
static void vp_outw(uint16_t off, uint16_t v)  { outw((uint16_t)(s_vnet.io + off), v); }
static void vp_outl(uint16_t off, uint32_t v)  { outl((uint16_t)(s_vnet.io + off), v); }

static void add_status(uint8_t bits)
{
    vp_outb(VIRTIO_PCI_STATUS, (uint8_t)(vp_inb(VIRTIO_PCI_STATUS) | bits));
}

/* Mask the device's legacy INTx line at the 8259 PIC.  This driver polls the
 * virtqueues, so it registers no IRQ handler; legacy virtio's level-triggered
 * INTx would otherwise stay asserted (we never read the ISR to clear it) and
 * storm the CPU into a livelock.  The boot PIC init unmasks every line, so we
 * must explicitly mask ours to stay panic-proof. */
static void mask_device_irq(uint8_t irq)
{
    if (irq == 0 || irq >= 16)
        return;                       /* 0 = no/invalid line; 2 = cascade */
    uint16_t port = (irq < 8) ? 0x21u : 0xA1u;
    uint8_t  bit  = (irq < 8) ? irq : (uint8_t)(irq - 8);
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

static uint32_t dma_alloc_pages(unsigned order)
{
    uint32_t skipped[16];
    uint32_t skipped_count = 0;
    uint32_t phys = PMM_ALLOC_ERROR;

    for (uint32_t tries = 0; tries < 64; tries++) {
        phys = pmm_alloc_pages(order);
        if (phys == PMM_ALLOC_ERROR)
            break;
        if (phys >= VIRTIO_NET_DMA_MIN_PHYS)
            break;
        if (skipped_count < (uint32_t)(sizeof(skipped) / sizeof(skipped[0]))) {
            skipped[skipped_count++] = phys;
        } else {
            pmm_free_pages(phys, order);
            phys = PMM_ALLOC_ERROR;
            break;
        }
        phys = PMM_ALLOC_ERROR;
    }

    for (uint32_t i = 0; i < skipped_count; i++)
        pmm_free_pages(skipped[i], order);

    return phys;
}

static int queue_alloc(virtio_queue_t *q, uint16_t qsel)
{
    vp_outw(VIRTIO_PCI_QUEUE_SEL, qsel);
    uint16_t size = vp_inw(VIRTIO_PCI_QUEUE_NUM);
    if (size == 0 || size > VIRTIO_NET_MAX_QSZ)
        return -1;

    uint32_t desc_bytes = sizeof(virtq_desc_t) * size;
    uint32_t avail_bytes = 4u + 2u * size;
    uint32_t used_bytes = 4u + sizeof(virtq_used_elem_t) * size;
    uint32_t used_off = align_up(desc_bytes + avail_bytes, PMM_FRAME_SIZE);
    uint32_t total = used_off + used_bytes;
    unsigned order = order_for_bytes(total);
    if (order >= PMM_MAX_ORDER)
        return -1;

    uint32_t phys = dma_alloc_pages(order);
    if (phys == PMM_ALLOC_ERROR)
        return -1;

    memset((void *)(uintptr_t)phys, 0, (1u << order) * PMM_FRAME_SIZE);

    q->qsel = qsel;
    q->size = size;
    q->phys = phys;
    q->bytes = (1u << order) * PMM_FRAME_SIZE;
    q->desc = (virtq_desc_t *)(uintptr_t)phys;
    q->avail = (virtq_avail_t *)(uintptr_t)(phys + desc_bytes);
    q->used = (virtq_used_t *)(uintptr_t)(phys + used_off);
    q->last_used = 0;

    /* Polled: ask the device not to raise used-ring interrupts. */
    q->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;

    vp_outw(VIRTIO_PCI_QUEUE_SEL, qsel);
    vp_outl(VIRTIO_PCI_QUEUE_PFN, phys >> 12);

    return 0;
}

static int alloc_buffers(void)
{
    s_vnet.rx_slots = (uint16_t)(s_vnet.rxq.size / 2);
    if (s_vnet.rx_slots == 0 || s_vnet.txq.size < 2)
        return -1;

    uint32_t rx_bytes = s_vnet.rx_slots * VIRTIO_NET_RX_BUF_SIZE;
    unsigned rx_order = order_for_bytes(rx_bytes);
    if (rx_order >= PMM_MAX_ORDER)
        return -1;
    s_vnet.rx_buf_phys = dma_alloc_pages(rx_order);
    if (s_vnet.rx_buf_phys == PMM_ALLOC_ERROR)
        return -1;
    s_vnet.rx_bufs = (uint8_t *)(uintptr_t)s_vnet.rx_buf_phys;
    memset(s_vnet.rx_bufs, 0, (1u << rx_order) * PMM_FRAME_SIZE);

    unsigned tx_order = order_for_bytes(VIRTIO_NET_TX_BUF_SIZE);
    if (tx_order >= PMM_MAX_ORDER)
        return -1;
    s_vnet.tx_buf_phys = dma_alloc_pages(tx_order);
    if (s_vnet.tx_buf_phys == PMM_ALLOC_ERROR)
        return -1;
    s_vnet.tx_buf = (uint8_t *)(uintptr_t)s_vnet.tx_buf_phys;
    memset(s_vnet.tx_buf, 0, (1u << tx_order) * PMM_FRAME_SIZE);
    return 0;
}

static void rx_refill_all(void)
{
    virtio_queue_t *q = &s_vnet.rxq;
    for (uint16_t i = 0; i < s_vnet.rx_slots; i++) {
        uint32_t phys = s_vnet.rx_buf_phys + (uint32_t)i * VIRTIO_NET_RX_BUF_SIZE;
        uint16_t head = (uint16_t)(i * 2);
        uint16_t body = (uint16_t)(head + 1);

        q->desc[head].addr_lo = phys;
        q->desc[head].addr_hi = 0;
        q->desc[head].len = sizeof(virtio_net_hdr_t);
        q->desc[head].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
        q->desc[head].next = body;

        q->desc[body].addr_lo = phys + sizeof(virtio_net_hdr_t);
        q->desc[body].addr_hi = 0;
        q->desc[body].len = VIRTIO_NET_RX_BUF_SIZE - sizeof(virtio_net_hdr_t);
        q->desc[body].flags = VIRTQ_DESC_F_WRITE;
        q->desc[body].next = 0;

        q->avail->ring[i] = head;
    }
    q->avail->idx = s_vnet.rx_slots;
    vq_barrier();
    vp_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->qsel);
}

static int virtio_net_probe(pci_device_t *dev)
{
    uint32_t io = pci_bar_io(dev, 0);
    if (io == 0)
        return -1;

    memset(&s_vnet, 0, sizeof(s_vnet));
    s_vnet.io = (uint16_t)io;

    pci_enable_bus_master(dev);
    /* Polled driver: mask our INTx line so the device can't livelock the CPU. */
    mask_device_irq(dev->irq_line);

    vp_outb(VIRTIO_PCI_STATUS, 0);
    add_status(VIRTIO_STATUS_ACKNOWLEDGE);
    add_status(VIRTIO_STATUS_DRIVER);

    uint32_t host_features = vp_inl(VIRTIO_PCI_HOST_FEATURES);
    uint32_t guest_features = host_features & VIRTIO_NET_F_MAC;
    vp_outl(VIRTIO_PCI_GUEST_FEATURES, guest_features);
    add_status(VIRTIO_STATUS_FEATURES_OK);
    if ((vp_inb(VIRTIO_PCI_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        add_status(VIRTIO_STATUS_FAILED);
        return -1;
    }

    if ((guest_features & VIRTIO_NET_F_MAC) == 0) {
        add_status(VIRTIO_STATUS_FAILED);
        return -1;
    }
    for (uint16_t i = 0; i < 6; i++)
        s_vnet.mac[i] = vp_inb((uint16_t)(VIRTIO_PCI_CONFIG + i));

    if (queue_alloc(&s_vnet.rxq, VIRTIO_NET_Q_RX) != 0 ||
        queue_alloc(&s_vnet.txq, VIRTIO_NET_Q_TX) != 0 ||
        alloc_buffers() != 0) {
        add_status(VIRTIO_STATUS_FAILED);
        return -1;
    }

    add_status(VIRTIO_STATUS_DRIVER_OK);
    s_vnet.up = 1;
    netdev_register(&virtio_net_ops);
    /* Post RX buffers only after DRIVER_OK so the notify is valid per spec. */
    rx_refill_all();

    return 0;
}

static const pci_driver_t virtio_net_driver = {
    .name = "virtio-net",
    .vendor = VIRTIO_PCI_VENDOR,
    .device = VIRTIO_PCI_NET_DEVICE,
    .class_code = 0,
    .subclass = 0,
    .match_class = 0,
    .probe = virtio_net_probe,
};

void virtio_net_register(void)
{
    pci_register_driver(&virtio_net_driver);
}

int virtio_net_present(void)
{
    return s_vnet.up;
}

const uint8_t *virtio_net_mac(void)
{
    return s_vnet.up ? s_vnet.mac : 0;
}

int virtio_net_send(const void *frame, uint16_t len)
{
    if (!s_vnet.up || !frame || len == 0)
        return -1;
    if ((uint32_t)len + sizeof(virtio_net_hdr_t) > VIRTIO_NET_TX_BUF_SIZE)
        return -1;

    virtio_queue_t *q = &s_vnet.txq;

    /* virtio_net_hdr (zeroed: no checksum offload, no GSO) + frame, as a
     * legacy-safe two-descriptor chain: header desc -> frame desc. */
    memset(s_vnet.tx_buf, 0, sizeof(virtio_net_hdr_t));
    memcpy(s_vnet.tx_buf + sizeof(virtio_net_hdr_t), frame, len);

    q->desc[0].addr_lo = s_vnet.tx_buf_phys;
    q->desc[0].addr_hi = 0;
    q->desc[0].len     = sizeof(virtio_net_hdr_t);
    q->desc[0].flags   = VIRTQ_DESC_F_NEXT;
    q->desc[0].next    = 1;

    q->desc[1].addr_lo = s_vnet.tx_buf_phys + sizeof(virtio_net_hdr_t);
    q->desc[1].addr_hi = 0;
    q->desc[1].len     = len;
    q->desc[1].flags   = 0;
    q->desc[1].next    = 0;

    uint16_t avail = q->avail->idx;
    q->avail->ring[avail % q->size] = 0;
    vq_barrier();                       /* publish the ring slot before idx */
    q->avail->idx = (uint16_t)(avail + 1);
    vq_barrier();
    vp_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->qsel);

    /* Fire-and-forget.  Under QEMU/TCG the device drains the TX queue on its
     * own thread once the guest yields the CPU; busy-waiting on the used ring
     * here would starve that backend and never complete.  One outstanding TX
     * buffer suffices for today's single-frame callers; a TX descriptor ring
     * with lazy reclaim lands alongside lwIP. */
    return 0;
}

int virtio_net_rx_poll(void *buf, uint16_t bufsz)
{
    if (!s_vnet.up || !buf)
        return -1;

    virtio_queue_t *q = &s_vnet.rxq;
    volatile virtq_used_t *used = (volatile virtq_used_t *)q->used;
    if (q->last_used == used->idx)
        return 0;

    volatile virtq_used_elem_t *e = &used->ring[q->last_used % q->size];
    uint16_t id = (uint16_t)e->id;
    uint32_t len = e->len;
    q->last_used++;

    if (id >= q->size || (id & 1u) || len <= sizeof(virtio_net_hdr_t)) {
        if (id < q->size && !(id & 1u)) {
            uint16_t avail = q->avail->idx;
            q->avail->ring[avail % q->size] = id;
            q->avail->idx = (uint16_t)(avail + 1);
            vq_barrier();
            vp_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->qsel);
        }
        return -1;
    }

    uint16_t slot = (uint16_t)(id / 2);
    if (slot >= s_vnet.rx_slots)
        return -1;

    uint8_t *pkt = s_vnet.rx_bufs + (uint32_t)slot * VIRTIO_NET_RX_BUF_SIZE;
    uint32_t frame_len = len - sizeof(virtio_net_hdr_t);
    uint32_t copy_len = frame_len < bufsz ? frame_len : bufsz;
    memcpy(buf, pkt + sizeof(virtio_net_hdr_t), copy_len);

    uint16_t avail = q->avail->idx;
    q->avail->ring[avail % q->size] = id;
    q->avail->idx = (uint16_t)(avail + 1);
    vq_barrier();
    vp_outw(VIRTIO_PCI_QUEUE_NOTIFY, q->qsel);

    return (int)frame_len;
}
