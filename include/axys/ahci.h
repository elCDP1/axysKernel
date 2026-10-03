#ifndef AXYS_AHCI_H
#define AXYS_AHCI_H

#include "axys/types.h"

/* AHCI (SATA) controller driver.
 *
 * Polled, not interrupt driven: the HBA's own registers are read in a spin
 * until the command finishes, so no MSI/MSI-X, no GSI routing and no interrupt
 * handler is involved. That is the trade -- a request costs a busy wait instead
 * of a wakeup, which is the right trade for a kernel whose only consumer is the
 * persistence flusher.
 *
 * One data FIS is in flight at a time, which caps a single command at 128
 * sectors; larger requests are split by the disk layer above. */

#define AXYS_AHCI_SECTOR_SIZE 512u

/* Detect the controller and the first drive behind it. Returns 0 when a usable
 * drive was found, -1 otherwise (the caller should then try ATA PIO). */
int axys_ahci_init(void);

int axys_ahci_present(void);

/* One-line boot-log summary. Never AXYS_NULL. */
const char *axys_ahci_summary(void);

/* Capacity in 512-byte sectors as a 64-bit LBA48 count, 0 when absent. */
axys_uint64_t axys_ahci_sectors(void);

int axys_ahci_read(axys_uint64_t lba, axys_uint32_t count, void *buffer);
int axys_ahci_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer);
int axys_ahci_flush(void);

#endif
