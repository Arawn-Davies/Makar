#ifndef _KERNEL_VIRTIO_NET_H
#define _KERNEL_VIRTIO_NET_H

#include <stdint.h>

/* Legacy virtio-net (transitional PCI device 1AF4:1000) driver.
 *
 * Polled split-ring transport over the legacy I/O-BAR register window.  This
 * is the raw Ethernet frame in/out layer; a TCP/IP stack (lwIP) sits on top
 * via the four entry points below.  No IRQ yet -- rx is drained by polling
 * virtio_net_rx_poll() from the network poll loop.
 */

/* Register the PCI driver.  Call before pci_probe_all() so the device binds. */
void virtio_net_register(void);

/* 1 once a device has been bound and brought up, 0 otherwise. */
int  virtio_net_present(void);

/* The device MAC (6 bytes), or NULL if no device is up. */
const uint8_t *virtio_net_mac(void);

/* Transmit one Ethernet frame (without the virtio_net_hdr; the driver
 * prepends it).  Returns 0 on success, negative on error/timeout. */
int  virtio_net_send(const void *frame, uint16_t len);

/* Poll the receive ring for one frame.  Copies up to bufsz bytes of the
 * Ethernet frame (virtio_net_hdr already stripped) into buf and returns the
 * frame length, 0 if nothing is pending, negative on error. */
int  virtio_net_rx_poll(void *buf, uint16_t bufsz);

#endif /* _KERNEL_VIRTIO_NET_H */
