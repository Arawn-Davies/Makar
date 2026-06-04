#ifndef _DRIVER_RTL8139_H
#define _DRIVER_RTL8139_H

#include <stdint.h>
#include <kernel/pci.h>

#define RTL8139_VENDOR  0x10EC
#define RTL8139_DEVICE  0x8139

void rtl8139_register(void);

#endif
