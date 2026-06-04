#include <kernel/netdev.h>

static const netdev_ops_t *s_netdev;

void netdev_register(const netdev_ops_t *ops)
{
    if (!ops || s_netdev)
        return;
    s_netdev = ops;
}

int netdev_present(void)
{
    return s_netdev && s_netdev->present && s_netdev->present();
}

const char *netdev_name(void)
{
    return s_netdev ? s_netdev->name : 0;
}

const uint8_t *netdev_mac(void)
{
    if (!netdev_present() || !s_netdev->mac)
        return 0;
    return s_netdev->mac();
}

int netdev_send(const void *frame, uint16_t len)
{
    if (!netdev_present() || !s_netdev->send)
        return -1;
    return s_netdev->send(frame, len);
}

int netdev_rx_poll(void *buf, uint16_t bufsz)
{
    if (!netdev_present() || !s_netdev->rx_poll)
        return -1;
    return s_netdev->rx_poll(buf, bufsz);
}
