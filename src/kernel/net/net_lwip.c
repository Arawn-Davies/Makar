#include <kernel/net_lwip.h>
#include <kernel/netdev.h>
#include <kernel/serial.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <lwip/dhcp.h>
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
static int s_dhcp_attempted;
static int s_dhcp_enabled;
static int s_dhcp_bound;
static int s_static_fallback;
static int s_released;       /* DHCP released: interface deconfigured, no IP */

/*
 * Net big-lock.  lwIP is built NO_SYS (single-threaded, not reentrant): the net
 * task drives it from net_lwip_poll(), while ring-3 tasks reach it through the
 * info/control/resolve syscalls.  Under preemptive syscalls those can interleave
 * with a poll mid-operation and corrupt lwIP state, so all public entry points
 * serialize on this cooperative lock (busy flag + yield, like the IDE lock).
 * The internal *_poll_ready/_try_dhcp helpers do NOT take it -- the public entry
 * holding it calls them, so a DHCP renew / DNS resolve can still pump lwIP while
 * the net task waits.  No-op while syscalls are still serial; load-bearing once
 * preemption is on. */
static volatile int s_net_busy;

static inline uint32_t net_irq_save(void)
{ uint32_t f; __asm__ volatile("pushfl; popl %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void net_irq_restore(uint32_t f)
{ __asm__ volatile("pushl %0; popfl" :: "r"(f) : "memory", "cc"); }

static void net_lock(void)
{
    for (;;) {
        uint32_t f = net_irq_save();
        if (!s_net_busy) { s_net_busy = 1; net_irq_restore(f); return; }
        net_irq_restore(f);
        task_yield();
    }
}
static void net_unlock(void)
{
    uint32_t f = net_irq_save();
    s_net_busy = 0;
    net_irq_restore(f);
}

static void net_lwip_note_dhcp_off(void)
{
    s_dhcp_enabled = 0;
    s_dhcp_bound = 0;
    s_static_fallback = 1;
}

static void net_lwip_set_static_slirp(void)
{
    ip4_addr_t ip, mask, gw, dns0;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    IP4_ADDR(&dns0, 10, 0, 2, 3);

    netif_set_addr(&s_netif, &ip, &mask, &gw);
    dns_setserver(0, (const ip_addr_t *)&dns0);
    s_released = 0;
}

/* Tear down all addressing: a real DHCP release leaves the interface with no
 * IPv4 address, gateway, or DNS server until the next renew/static config. */
static void net_lwip_clear_config(void)
{
    ip4_addr_t any;
    IP4_ADDR(&any, 0, 0, 0, 0);
    netif_set_addr(&s_netif, &any, &any, &any);
    dns_setserver(0, (const ip_addr_t *)&any);
}

static void net_lwip_poll_ready(void)
{
    makar_netif_poll(&s_netif);
    sys_check_timeouts();
    if (s_dhcp_enabled && dhcp_supplied_address(&s_netif))
        s_dhcp_bound = 1;
}

static int net_lwip_try_dhcp(uint32_t wait_ticks, int log_result)
{
    s_dhcp_attempted = 1;
    s_dhcp_bound = 0;
    s_static_fallback = 0;
    s_released = 0;

    if (dhcp_start(&s_netif) != ERR_OK) {
        net_lwip_note_dhcp_off();
        net_lwip_set_static_slirp();
        if (log_result)
            Serial_WriteString("lwip: DHCP start failed, using static 10.0.2.15/24 gw=10.0.2.2 dns=10.0.2.3\n");
        return 0;
    }

    s_dhcp_enabled = 1;
    uint32_t deadline = timer_get_ticks() + wait_ticks;
    while (timer_get_ticks() < deadline && !s_dhcp_bound) {
        net_lwip_poll_ready();
        task_yield();
    }
    if (s_dhcp_bound) {
        if (log_result)
            Serial_WriteString("lwip: DHCP lease acquired\n");
        return 0;
    }

    dhcp_stop(&s_netif);
    net_lwip_note_dhcp_off();
    net_lwip_set_static_slirp();
    if (log_result)
        Serial_WriteString("lwip: DHCP timeout, using static 10.0.2.15/24 gw=10.0.2.2 dns=10.0.2.3\n");
    return 0;
}

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
    netif_set_link_up(&s_netif);
    dns_setserver(0, (const ip_addr_t *)&dns0);
    s_ready = 1;

    net_lwip_try_dhcp(250u, 1);
    return 0;
}

void net_lwip_poll(void)
{
    if (!s_ready)
        return;
    net_lock();
    net_lwip_poll_ready();
    net_unlock();
}

int net_lwip_ready(void)
{
    return s_ready;
}

typedef struct {
    ip_addr_t addr;
    int state;          /* 0 = pending, 1 = resolved, -1 = failed */
} net_resolve_t;

static void net_lwip_dns_found(const char *name, const ip_addr_t *ipaddr,
                               void *arg)
{
    (void)name;
    net_resolve_t *r = (net_resolve_t *)arg;
    if (ipaddr) {
        r->addr = *ipaddr;
        r->state = 1;
    } else {
        r->state = -1;          /* NXDOMAIN / lookup failed */
    }
}

int net_lwip_resolve(const char *host, uint8_t ip_out[4], uint32_t timeout_ticks)
{
    if (!s_ready || !host || !ip_out)
        return -1;

    net_lock();
    int rv = -1;
    net_resolve_t r;
    r.state = 0;
    ip_addr_t immediate;

    err_t rc = dns_gethostbyname(host, &immediate, net_lwip_dns_found, &r);
    if (rc == ERR_OK) {
        /* Dotted-quad literal or already cached: answer is in `immediate`. */
        r.addr = immediate;
        r.state = 1;
    } else if (rc == ERR_INPROGRESS) {
        uint32_t deadline = timer_get_ticks() + timeout_ticks;
        while (timer_get_ticks() < deadline && r.state == 0) {
            net_lwip_poll_ready();   /* we hold the net lock; pump lwIP ourselves */
            task_yield();
        }
    }

    if (r.state == 1) {
        const ip4_addr_t *v4 = ip_2_ip4(&r.addr);
        ip_out[0] = ip4_addr1(v4);
        ip_out[1] = ip4_addr2(v4);
        ip_out[2] = ip4_addr3(v4);
        ip_out[3] = ip4_addr4(v4);
        rv = 0;
    }
    net_unlock();
    return rv;
}

static void net_lwip_copy_ip4(const ip4_addr_t *src, uint8_t out[4])
{
    out[0] = ip4_addr1(src);
    out[1] = ip4_addr2(src);
    out[2] = ip4_addr3(src);
    out[3] = ip4_addr4(src);
}

int net_lwip_local_ip(uint8_t out[4])
{
    if (!s_ready || !out) return -1;
    net_lock();
    net_lwip_copy_ip4(netif_ip4_addr(&s_netif), out);
    net_unlock();
    return 0;
}

int net_lwip_gateway(uint8_t out[4])
{
    if (!s_ready || !out) return -1;
    net_lock();
    net_lwip_copy_ip4(netif_ip4_gw(&s_netif), out);
    net_unlock();
    return 0;
}

static int net_lwip_control_locked(int cmd)
{
    if (!s_ready)
        return -1;

    switch (cmd) {
    case NET_CTL_DHCP_RELEASE:
        dhcp_release_and_stop(&s_netif);
        s_dhcp_attempted = 1;
        s_dhcp_enabled = 0;
        s_dhcp_bound = 0;
        s_static_fallback = 0;
        s_released = 1;
        net_lwip_clear_config();
        Serial_WriteString("lwip: DHCP released, eth0 deconfigured (no address)\n");
        return 0;
    case NET_CTL_DHCP_RENEW:
        dhcp_release_and_stop(&s_netif);
        return net_lwip_try_dhcp(250u, 1);
    case NET_CTL_DNS_FLUSH: {
        const ip_addr_t *dns0 = dns_getserver(0);
        ip_addr_t keep_dns0 = *dns0;
        dns_clear_cache();
        dns_setserver(0, &keep_dns0);
        return 0;
    }
    default:
        return -1;
    }
}

int net_lwip_control(int cmd)
{
    net_lock();
    int r = net_lwip_control_locked(cmd);
    net_unlock();
    return r;
}

static void info_append(char *buf, uint32_t cap, uint32_t *off, const char *s)
{
    while (*s && *off < cap - 1)
        buf[(*off)++] = *s++;
}

static void info_append_dec(char *buf, uint32_t cap, uint32_t *off, uint32_t v)
{
    char tmp[10];
    uint32_t n = 0;
    if (v == 0) {
        if (*off < cap - 1)
            buf[(*off)++] = '0';
        return;
    }
    while (v && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n && *off < cap - 1)
        buf[(*off)++] = tmp[--n];
}

static void info_append_ip4(char *buf, uint32_t cap, uint32_t *off,
                            const ip4_addr_t *ip)
{
    info_append_dec(buf, cap, off, ip4_addr1(ip));
    info_append(buf, cap, off, ".");
    info_append_dec(buf, cap, off, ip4_addr2(ip));
    info_append(buf, cap, off, ".");
    info_append_dec(buf, cap, off, ip4_addr3(ip));
    info_append(buf, cap, off, ".");
    info_append_dec(buf, cap, off, ip4_addr4(ip));
}

static void info_append_mac(char *buf, uint32_t cap, uint32_t *off,
                            const uint8_t *mac)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i && *off < cap - 1)
            buf[(*off)++] = '-';
        if (*off < cap - 1)
            buf[(*off)++] = h[(mac[i] >> 4) & 0xF];
        if (*off < cap - 1)
            buf[(*off)++] = h[mac[i] & 0xF];
    }
}

static int net_lwip_info_locked(char *buf, uint32_t cap)
{
    if (!buf || cap == 0)
        return 0;
    uint32_t off = 0;

    info_append(buf, cap, &off, "Makar Network Configuration\n\n");
    if (!netdev_present()) {
        info_append(buf, cap, &off, "Ethernet adapters: none\n");
        buf[off] = '\0';
        return (int)off;
    }

    info_append(buf, cap, &off, "Ethernet adapter eth0:\n");
    info_append(buf, cap, &off, "   Driver . . . . . . . . . . : ");
    info_append(buf, cap, &off, netdev_name() ? netdev_name() : "unknown");
    info_append(buf, cap, &off, "\n");
    info_append(buf, cap, &off, "   Link State . . . . . . . . : ");
    info_append(buf, cap, &off, s_ready && netif_is_link_up(&s_netif) ? "up\n" : "down\n");
    info_append(buf, cap, &off, "   Physical Address. . . . . . : ");
    if (netdev_mac())
        info_append_mac(buf, cap, &off, netdev_mac());
    else
        info_append(buf, cap, &off, "unknown");
    info_append(buf, cap, &off, "\n");
    info_append(buf, cap, &off, "   DHCP Enabled. . . . . . . . : ");
    info_append(buf, cap, &off, s_dhcp_attempted ? "yes\n" : "no\n");
    info_append(buf, cap, &off, "   DHCP State. . . . . . . . . : ");
    if (s_released)
        info_append(buf, cap, &off, "released\n");
    else if (s_dhcp_bound)
        info_append(buf, cap, &off, "bound\n");
    else if (s_static_fallback)
        info_append(buf, cap, &off, "fallback-static\n");
    else if (s_dhcp_enabled)
        info_append(buf, cap, &off, "probing\n");
    else
        info_append(buf, cap, &off, "off\n");

    if (s_ready) {
        const ip_addr_t *dns0 = dns_getserver(0);
        info_append(buf, cap, &off, "   IPv4 Address. . . . . . . . : ");
        info_append_ip4(buf, cap, &off, netif_ip4_addr(&s_netif));
        info_append(buf, cap, &off, "\n");
        info_append(buf, cap, &off, "   Subnet Mask . . . . . . . . : ");
        info_append_ip4(buf, cap, &off, netif_ip4_netmask(&s_netif));
        info_append(buf, cap, &off, "\n");
        info_append(buf, cap, &off, "   Default Gateway . . . . . . : ");
        info_append_ip4(buf, cap, &off, netif_ip4_gw(&s_netif));
        info_append(buf, cap, &off, "\n");
        info_append(buf, cap, &off, "   DNS Servers . . . . . . . . : ");
        info_append_ip4(buf, cap, &off, ip_2_ip4(dns0));
        info_append(buf, cap, &off, "\n");
    }

    buf[off] = '\0';
    return (int)off;
}

int net_lwip_info(char *buf, uint32_t cap)
{
    net_lock();
    int r = net_lwip_info_locked(buf, cap);
    net_unlock();
    return r;
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
