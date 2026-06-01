#ifndef _DRIVER_USB_H
#define _DRIVER_USB_H

#include <stdint.h>
#include <kernel/pci.h>

#define USB_CLASS     0x0C
#define USB_SUBCLASS  0x03
#define USB_PI_UHCI   0x00
#define USB_PI_OHCI   0x10
#define USB_PI_EHCI   0x20
#define USB_PI_XHCI   0x30

/* TODO: controller init, port reset, device enumeration, HID class */
void usb_init(void);

#endif
