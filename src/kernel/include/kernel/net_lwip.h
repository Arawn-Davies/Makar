#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H

#include <stdint.h>

int net_lwip_init(void);
void net_lwip_task(void);
void net_lwip_poll(void);
int net_lwip_ready(void);
int net_lwip_info(char *buf, uint32_t cap);
int net_lwip_control(int cmd);

#define NET_CTL_DHCP_RELEASE 1
#define NET_CTL_DHCP_RENEW   2
#define NET_CTL_DNS_FLUSH    3

#endif
