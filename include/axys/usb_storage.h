#ifndef AXYS_USB_STORAGE_H
#define AXYS_USB_STORAGE_H

#include "axys/types.h"

/*
 * USB Mass Storage (Bulk-Only Transport) over one xHCI-configured drive:
 * TEST UNIT READY, READ CAPACITY(10), READ(10)/WRITE(10) with CBW/CSW framing
 * and reset recovery. Only 512-byte blocks are supported.
 */

/* Find the drive, wait for it to become ready and read its capacity. Returns
 * 0 with sectors known, or -1 (no drive). Idempotent. */
int axys_usb_storage_init(void);

/* Capacity in 512-byte sectors, 0 when absent. */
axys_uint64_t axys_usb_storage_sectors(void);

/* Sector I/O. Both return 0 or -1 (range, timeout, stall, residue). */
int axys_usb_storage_read(axys_uint64_t lba, axys_uint32_t count, void *buffer);
int axys_usb_storage_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer);

#endif
