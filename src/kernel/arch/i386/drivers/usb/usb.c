/* usb.c -- USB host controller detection.
 *
 * PCI class 0x0C subclass 0x03:
 *   prog_if 0x00 = UHCI  (Intel, 1.x)
 *   prog_if 0x10 = OHCI  (open HCI, 1.x)
 *   prog_if 0x20 = EHCI  (2.0)
 *   prog_if 0x30 = xHCI  (3.x, USB 3.0+)
 *
 * Today: detect controllers via the PCI scan and report them.  Device
 * enumeration + a HID class driver (keyboard/mouse boot protocol) are the
 * next slices; input is still PS/2 until then.  See CLAUDE.roadmap.md.
 */
#include "usb.h"
#include <kernel/tty.h>

static const char *usb_kind(uint8_t prog_if)
{
    switch (prog_if) {
        case USB_PI_UHCI: return "UHCI (USB 1.1)";
        case USB_PI_OHCI: return "OHCI (USB 1.1)";
        case USB_PI_EHCI: return "EHCI (USB 2.0)";
        case USB_PI_XHCI: return "xHCI (USB 3.x)";
        default:          return "USB (unknown HCI)";
    }
}

void usb_init(void)
{
    int found = 0;
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];
        if (d->class_code == USB_CLASS && d->subclass == USB_SUBCLASS) {
            found++;
            t_writestring("  usb: ");
            t_writestring(usb_kind(d->prog_if));
            t_writestring(" host controller\n");
        }
    }
    if (!found)
        t_writestring("  usb: no host controllers present\n");
    /* HID enumeration not yet implemented -- keyboard/mouse remain on PS/2. */
}
