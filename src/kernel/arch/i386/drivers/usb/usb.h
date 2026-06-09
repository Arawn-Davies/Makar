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

/* USB HID boot-protocol report decoders -- route into the shared input layer
 * (mouse_post_event / the keycode ring) so input is not tied to PS/2.  Called by
 * the (future) HCI poll loop with an 8-byte boot report; the mouse path is
 * functional, the keyboard path covers the common US keys.  See usb.c. */
void usb_hid_mouse_report(const uint8_t *report8);
void usb_hid_keyboard_report(const uint8_t *report8);

#endif
