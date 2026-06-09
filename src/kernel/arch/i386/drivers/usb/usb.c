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
#include <kernel/mouse.h>
#include <kernel/keyboard.h>

/* ---- USB HID boot-protocol decoders ------------------------------------- *
 * These turn the fixed-layout boot reports into Makar input events, feeding the
 * SAME sinks PS/2 does (mouse_post_event / the keycode ring) -- so input is not
 * tied to PS/2.  A USB HCI + enumeration driver (the next slice) will SET_PROTO
 * (boot), poll the interrupt-IN endpoint, and hand each report here.  Until the
 * HCI lands these are exercised only by the boilerplate; PS/2 stays the live
 * source.  (Linux-style: usbcore + hid-generic boot protocol routed into the shared
 * input layer.) */

/* HID boot mouse: byte0 buttons (b0 L, b1 R, b2 M), byte1 dx, byte2 dy (both
 * signed, +y already screen-down), byte3 wheel. */
void usb_hid_mouse_report(const uint8_t *r)
{
    if (!r) return;
    mouse_post_event((int)(int8_t)r[1], (int)(int8_t)r[2], r[0] & 0x07);
}

/* HID boot keyboard: byte0 modifier bitmap, byte1 reserved, byte2..7 up to six
 * pressed USB usage codes.  Edge-detected against the previous report so each
 * new key fires once; routed into the keycode ring via keyboard_inject_key
 * (set-1 keycodes).  Usage->keycode mapping is the minimal US set for now. */
static uint8_t hid_usage_to_kc(uint8_t u)
{
    /* USB HID usage (0x04='a'..) -> Makar set-1 keycode.  Letters/digits/space/
     * enter/esc/backspace/tab cover the common path; extend as needed. */
    static const uint8_t a2kc[] = {  /* indexed by usage-0x04 for 'a'..'z' */
        0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    if (u >= 0x04 && u <= 0x1D) return a2kc[u - 0x04];     /* a..z */
    if (u >= 0x1E && u <= 0x26) return (uint8_t)(0x02 + (u - 0x1E)); /* 1..9 */
    if (u == 0x27) return 0x0B;   /* 0 */
    if (u == 0x28) return 0x1C;   /* enter */
    if (u == 0x29) return 0x01;   /* esc */
    if (u == 0x2A) return 0x0E;   /* backspace */
    if (u == 0x2B) return 0x0F;   /* tab */
    if (u == 0x2C) return 0x39;   /* space */
    return 0;
}

void usb_hid_keyboard_report(const uint8_t *r)
{
    static uint8_t prev[6];
    if (!r) return;
    uint8_t mod = r[0];
    int shift = (mod & 0x22) != 0, ctrl = (mod & 0x11) != 0, alt = (mod & 0x44) != 0;
    for (int i = 0; i < 6; i++) {
        uint8_t u = r[2 + i];
        if (!u) continue;
        int was = 0;
        for (int j = 0; j < 6; j++) if (prev[j] == u) { was = 1; break; }
        if (was) continue;                       /* still held -- not a new press */
        uint8_t kc = hid_usage_to_kc(u);
        if (kc) keyboard_inject_key(kc, shift, ctrl, alt);
    }
    for (int i = 0; i < 6; i++) prev[i] = r[2 + i];
}

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
    /* HID boot-protocol decoders (usb_hid_{mouse,keyboard}_report) feed the
     * shared input layer; a USB HCI + enumeration driver to actually pump them
     * is the next slice.  Until then input stays on PS/2. */
    if (found)
        t_writestring("  usb: HID boot input ready (enumeration pending)\n");
}
