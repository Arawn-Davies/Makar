#ifndef _KERNEL_NET_LWIP_H
#define _KERNEL_NET_LWIP_H

#include <stdint.h>

int net_lwip_init(void);
void net_lwip_task(void);
void net_lwip_poll(void);
int net_lwip_ready(void);

#endif
