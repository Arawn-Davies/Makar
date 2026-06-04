#ifndef _KERNEL_NETDEV_H
#define _KERNEL_NETDEV_H

#include <stdint.h>

typedef struct {
    const char *name;
    int (*present)(void);
    const uint8_t *(*mac)(void);
    int (*send)(const void *frame, uint16_t len);
    int (*rx_poll)(void *buf, uint16_t bufsz);
} netdev_ops_t;

void netdev_register(const netdev_ops_t *ops);

int netdev_present(void);
const char *netdev_name(void);
const uint8_t *netdev_mac(void);
int netdev_send(const void *frame, uint16_t len);
int netdev_rx_poll(void *buf, uint16_t bufsz);

#endif /* _KERNEL_NETDEV_H */
