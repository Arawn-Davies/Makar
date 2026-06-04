#ifndef _KERNEL_NET_DRIVERS_H
#define _KERNEL_NET_DRIVERS_H

void virtio_net_register(void);
void rtl8139_register(void);
void e1000_register(void);
void pcnet_register(void);

#endif /* _KERNEL_NET_DRIVERS_H */
