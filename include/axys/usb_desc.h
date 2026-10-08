#ifndef AXYS_USB_DESC_H
#define AXYS_USB_DESC_H

#include "axys/types.h"

/*
 * Parsing of USB configuration descriptors. The blob comes straight from the
 * device, so every field is untrusted: a descriptor may claim any length, be
 * truncated by the end of the blob, or be too short to contain the fields the
 * caller reads. Nothing in here reads outside [blob, blob + length).
 */

#define AXYS_USB_DESC_CONFIG 2u
#define AXYS_USB_DESC_INTERFACE 4u
#define AXYS_USB_DESC_ENDPOINT 5u

/* Offset of the `instance`-th descriptor of `type`, or -1. A descriptor of the
 * requested type that is shorter than its type requires (9 bytes for
 * configuration and interface, 7 for endpoint) ends the search: the caller is
 * about to read fixed offsets inside it. A zero- or one-byte length, or one
 * that runs past the blob, also ends the walk. */
int axys_usb_desc_find(const axys_uint8_t *blob, axys_size_t length, axys_uint8_t type, int instance);

/* Offset just past the endpoints of the interface descriptor at `iface_off`:
 * the next interface descriptor, or `length` when it is the last one. */
axys_size_t axys_usb_desc_interface_end(const axys_uint8_t *blob, axys_size_t length, int iface_off);

struct axys_usb_kbd_desc {
    axys_uint8_t config_value;
    axys_uint8_t iface;
    axys_uint8_t ep_addr; /* interrupt IN endpoint address */
    axys_uint16_t mps;    /* 1..64, defaulted to 8 when the device reports otherwise */
    axys_uint8_t interval;
};

/* First HID boot-protocol keyboard interface (3/1/1) and its interrupt-IN
 * endpoint (which must belong to that interface). Returns 0 or -1. */
int axys_usb_desc_keyboard(const axys_uint8_t *blob, axys_size_t length, struct axys_usb_kbd_desc *out);

struct axys_usb_msc_desc {
    axys_uint8_t config_value;
    axys_uint8_t iface;
    axys_uint8_t out_ep, in_ep;     /* endpoint addresses */
    axys_uint16_t out_mps, in_mps;  /* 8..1024 */
};

/* First Bulk-Only mass-storage interface (8/6/0x50) with exactly one bulk-IN
 * and one bulk-OUT endpoint of its own. Returns 0 or -1. */
int axys_usb_desc_storage(const axys_uint8_t *blob, axys_size_t length, struct axys_usb_msc_desc *out);

#endif
