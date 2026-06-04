#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H

#include <stdint.h>

int net_lwip_init(void);
void net_lwip_task(void);
void net_lwip_poll(void);
int net_lwip_ready(void);
int net_lwip_info(char *buf, uint32_t cap);
int net_lwip_control(int cmd);

/* Resolve a hostname (or dotted-quad string) to an IPv4 address via lwIP DNS.
 * Blocks (yielding) up to timeout_ticks for the lookup.  Writes the 4 address
 * octets into ip_out and returns 0 on success, -1 on failure/timeout. */
int net_lwip_resolve(const char *host, uint8_t ip_out[4], uint32_t timeout_ticks);

/* Copy the interface's current IPv4 address / default gateway into out[4]
 * (whatever DHCP or the static fallback assigned -- no address is assumed).
 * Returns 0 on success, -1 if the stack is not ready. */
int net_lwip_local_ip(uint8_t out[4]);
int net_lwip_gateway(uint8_t out[4]);

#define NET_CTL_DHCP_RELEASE 1
#define NET_CTL_DHCP_RENEW   2
#define NET_CTL_DNS_FLUSH    3

#endif
