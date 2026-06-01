/* usb.c -- USB host controller detection and enumeration (not yet implemented).
 *
 * PCI class 0x0C subclass 0x03:
 *   prog_if 0x00 = UHCI  (Intel, 1.x)
 *   prog_if 0x10 = OHCI  (open HCI, 1.x)
 *   prog_if 0x20 = EHCI  (2.0)
 *   prog_if 0x30 = xHCI  (3.x, USB 3.0+)
 *
 * Planned: detect controllers via PCI scan, enumerate root hubs,
 * expose HID keyboard/mouse as an alternative input path.
 */
#include "usb.h"
