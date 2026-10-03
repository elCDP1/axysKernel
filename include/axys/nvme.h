#ifndef AXYS_NVME_H
#define AXYS_NVME_H

#include "axys/types.h"

/* Polled PCIe NVMe driver. Supports one namespace with 512-byte logical
 * blocks and one outstanding command; unsupported controllers fail closed. */
int axys_nvme_init(void);
axys_uint64_t axys_nvme_sectors(void);
const char *axys_nvme_summary(void);
int axys_nvme_read(axys_uint64_t lba, axys_uint32_t count, void *buffer);
int axys_nvme_write(axys_uint64_t lba, axys_uint32_t count, const void *buffer);
int axys_nvme_flush(void);

#endif
