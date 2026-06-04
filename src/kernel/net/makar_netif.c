#include <kernel/netdev.h>
#include <lwip/etharp.h>
#include <lwip/ethip6.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <netif/ethernet.h>
#include <string.h>

#define MAKAR_NETIF_MTU 1500

static err_t makar_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    static uint8_t frame[1518];
    if (!p || p->tot_len > sizeof(frame))
        return ERR_BUF;

    if (pbuf_copy_partial(p, frame, p->tot_len, 0) != p->tot_len)
        return ERR_BUF;

    return netdev_send(frame, p->tot_len) == 0 ? ERR_OK : ERR_IF;
}

err_t makar_netif_init(struct netif *netif)
{
    const uint8_t *mac = netdev_mac();
    if (!netif || !mac)
        return ERR_IF;

    netif->name[0] = 'm';
    netif->name[1] = 'k';
    netif->output = etharp_output;
    netif->linkoutput = makar_linkoutput;
    netif->mtu = MAKAR_NETIF_MTU;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, ETH_HWADDR_LEN);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

int makar_netif_poll(struct netif *netif)
{
    static uint8_t frame[1536];
    int delivered = 0;

    for (;;) {
        int n = netdev_rx_poll(frame, sizeof(frame));
        if (n <= 0)
            break;

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p)
            continue;

        if (pbuf_take(p, frame, (u16_t)n) == ERR_OK) {
            if (netif->input(p, netif) == ERR_OK) {
                delivered++;
                continue;
            }
        }
        pbuf_free(p);
    }

    return delivered;
}
