#include <kernel/net_lwip.h>
#include <kernel/netdev.h>
#include <kernel/serial.h>
#include <kernel/task.h>
#include <lwip/init.h>
#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/timeouts.h>
#include <netif/ethernet.h>

err_t makar_netif_init(struct netif *netif);
int makar_netif_poll(struct netif *netif);

static struct netif s_netif;
static int s_ready;

int net_lwip_init(void)
{
    if (s_ready)
        return 0;
    if (!netdev_present()) {
        Serial_WriteString("lwip: no netdev, not starting\n");
        return -1;
    }

    lwip_init();

    ip4_addr_t ip, mask, gw, dns0;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    IP4_ADDR(&dns0, 10, 0, 2, 3);

    if (!netif_add(&s_netif, &ip, &mask, &gw, NULL, makar_netif_init, ethernet_input)) {
        Serial_WriteString("lwip: netif_add failed\n");
        return -1;
    }

    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    dns_setserver(0, (const ip_addr_t *)&dns0);
    s_ready = 1;
    Serial_WriteString("lwip: up 10.0.2.15/24 gw=10.0.2.2 dns=10.0.2.3\n");
    return 0;
}

void net_lwip_poll(void)
{
    if (!s_ready)
        return;
    makar_netif_poll(&s_netif);
    sys_check_timeouts();
}

int net_lwip_ready(void)
{
    return s_ready;
}

void net_lwip_task(void)
{
    if (net_lwip_init() != 0)
        return;
    for (;;) {
        net_lwip_poll();
        task_yield();
    }
}
