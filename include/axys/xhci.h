#ifndef AXYS_XHCI_H
#define AXYS_XHCI_H

#include "axys/types.h"

/*
 * xHCI USB host controller driver (polled) with USB HID keyboard support.
 *
 * Scope: one xHCI controller (PCI class 0c/03/0x30), full/low-speed devices
 * behind its root-hub ports, boot-protocol keyboards. Control transfers and
 * one interrupt-IN endpoint per keyboard; completions are polled from the
 * event ring by a kernel task, which decodes reports through kernel/usb_hid.c
 * into console input. No isochronous transfers, no hubs behind hubs, no USB
 * mass storage yet.
 */

/* Probe PCI, reset the controller and enumerate keyboards. Returns the number
 * of keyboards configured, 0 when there is no controller or no keyboard, or
 * -1 on a controller failure. Safe to call when no xHCI exists. */
int axys_xhci_init(void);

/* One-line summary of what was found, for the boot log. */
const char *axys_xhci_summary(void);

/* Keyboards currently delivering input. */
int axys_xhci_keyboard_count(void);

/* Bulk transport for one addressed slot: a single ≤4096-byte transfer on the
 * given endpoint DCI (2 = EP1 OUT, 3 = EP1 IN, ...). Returns 0 on success.
 * Short IN packets complete normally; anything else (stall, timeout, dead
 * controller) returns -1 with nothing queued. */
int axys_usb_bulk_transfer(axys_uint8_t slot, axys_uint8_t dci, axys_uint64_t data_phys,
                           axys_uint32_t length, int data_in, axys_uint32_t timeout_ms);

/* Class/vendor control transfer on an addressed slot (Get Max LUN, Bulk-Only
 * Reset, hub requests...). Follows the same rules as the internal control
 * path. Returns 0 on success. */
int axys_usb_control(axys_uint8_t slot, axys_uint8_t request_type, axys_uint8_t request,
                     axys_uint16_t value, axys_uint16_t index, axys_uint64_t data_phys,
                     axys_uint32_t length, int data_in, axys_uint32_t timeout_ms);

/* Reset (unstall) one endpoint of an addressed slot. Best effort. */
void axys_usb_reset_endpoint(axys_uint8_t slot, axys_uint8_t dci);

/* First configured Bulk-Only mass-storage slot, or 0 when none exists. */
int axys_usb_storage_slot(void);

/* Slot and bulk endpoint DCIs of the first configured drive. Returns 0. */
int axys_usb_storage_info(axys_uint8_t *slot, axys_uint8_t *dci_out, axys_uint8_t *dci_in);

#endif
