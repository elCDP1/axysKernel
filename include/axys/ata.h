#ifndef AXYS_ATA_H
#define AXYS_ATA_H

#include "axys/types.h"

/* ATA PIO driver for the primary channel's master drive. Kept as a fallback for
 * machines with no AHCI controller, and for the BIOS-legacy IDE mode where the
 * controller is not enumerable over PCI. LBA28 only, so the addressable range
 * stops at 128 GiB. */

/* Returns 0 if a usable drive exists, -1 otherwise. */
int axys_ata_init(void);
axys_uint32_t axys_ata_sectors(void);
int axys_ata_read(axys_uint32_t lba, axys_uint32_t count, void *buffer);
int axys_ata_write(axys_uint32_t lba, axys_uint32_t count, const void *buffer);
int axys_ata_flush(void);

#endif
