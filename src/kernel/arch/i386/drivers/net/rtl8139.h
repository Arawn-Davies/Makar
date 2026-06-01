#ifndef _DRIVER_RTL8139_H
#define _DRIVER_RTL8139_H

#include <stdint.h>
#include <kernel/pci.h>

#define RTL8139_VENDOR  0x10EC
#define RTL8139_DEVICE  0x8139

/* TODO: init, tx, rx, irq handler */
int  rtl8139_probe(const pci_device_t *dev);

#endif
