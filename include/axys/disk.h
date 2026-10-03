#ifndef AXYS_DISK_H
#define AXYS_DISK_H

#include "axys/types.h"

#define AXYS_SECTOR_SIZE 512u

/* The system disk, whichever controller drives it. axys_disk_init() prefers
 * NVMe, then AHCI/SATA, then primary-channel ATA PIO. All calls are serialized
 * internally and may block. */

/* Detect the drive and pick a backend. Returns 0 if a usable disk exists, -1
 * otherwise. */
int axys_disk_init(void);

/* Capacity in 512-byte sectors (0 when there is no disk), and the same value as
 * a full 64-bit LBA48 count for callers that want the real size. */
axys_uint32_t axys_disk_sectors(void);
axys_uint64_t axys_disk_capacity(void);

/* Which backend won: 0 none, 1 AHCI/SATA, 2 ATA PIO, 3 NVMe. */
int axys_disk_backend(void);
const char *axys_disk_backend_note(void);

/* Return 0 on success, -1 on an I/O error, timeout or out-of-range request. */
int axys_disk_read(axys_uint32_t lba, axys_uint32_t count, void *buffer);
int axys_disk_write(axys_uint32_t lba, axys_uint32_t count, const void *buffer);
int axys_disk_flush(void);

#endif
