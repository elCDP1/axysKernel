#include "axys/ahci.h"
#include "axys/ata.h"
#include "axys/disk.h"
#include "axys/nvme.h"
#include "axys/pci.h"
#include "axys/printf.h"
#include "axys/string.h"

/* The system disk, behind one interface.
 *
 * Two drivers can drive it: AHCI/SATA, which is what any real machine with a
 * modern controller exposes, and ATA PIO on the primary IDE channel, which is
 * what a machine in BIOS-legacy mode or a bare `-drive if=ide` gives. The rest
 * of the kernel -- persistence in particular -- only ever sees axys_disk_*, so
 * which one won is decided once at boot and never revisited.
 *
 * AHCI is tried first because it is the modern path and reports a 48-bit
 * capacity, while the PIO path is capped at LBA28. */

enum disk_backend {
    DISK_NONE = 0,
    DISK_AHCI = 1,
    DISK_ATA = 2,
    DISK_NVME = 3,
};

static enum disk_backend backend;
static axys_uint64_t capacity;
static char backend_note[200];

int axys_disk_init(void)
{
    if (backend != DISK_NONE) {
        return 0; /* already probed; keep the backend that answered */
    }
    backend_note[0] = '\0';
    capacity = 0u;

    if (axys_nvme_init() == 0) {
        backend = DISK_NVME;
        capacity = axys_nvme_sectors();
        axys_snprintf(backend_note, sizeof(backend_note), "nvme: %s", axys_nvme_summary());
        return 0;
    }
    if (axys_ahci_init() == 0) {
        backend = DISK_AHCI;
        capacity = axys_ahci_sectors();
        axys_snprintf(backend_note, sizeof(backend_note), "ahci: %s", axys_ahci_summary());
        return 0;
    }
    if (axys_ata_init() == 0) {
        backend = DISK_ATA;
        capacity = axys_ata_sectors();
        axys_snprintf(backend_note, sizeof(backend_note), "ata pio: %u sectors, LBA28",
                      (axys_uint32_t)capacity);
        return 0;
    }
    backend = DISK_NONE;
    axys_snprintf(backend_note, sizeof(backend_note), "nvme: %s; ahci: %s; ata pio: no drive",
                  axys_nvme_summary(), axys_ahci_summary());
    return -1;
}

int axys_disk_backend(void)
{
    return (int)backend;
}

const char *axys_disk_backend_note(void)
{
    return backend_note;
}

axys_uint64_t axys_disk_capacity(void)
{
    return capacity;
}

/* The public API is 32-bit in LBA, which is all the persistence format needs.
 * lba and count are already 32-bit parameters, so the only real question is
 * whether the request lies inside the drive. */
static int range_ok(axys_uint32_t lba, axys_uint32_t count)
{
    return backend != DISK_NONE && count != 0u && capacity != 0u &&
           (axys_uint64_t)lba < capacity && (axys_uint64_t)count <= capacity - lba;
}

axys_uint32_t axys_disk_sectors(void)
{
    return capacity > 0xffffffffULL ? 0xffffffffu : (axys_uint32_t)capacity;
}

int axys_disk_read(axys_uint32_t lba, axys_uint32_t count, void *buffer)
{
    if (!range_ok(lba, count)) {
        return -1;
    }
    if (backend == DISK_AHCI) {
        return axys_ahci_read(lba, count, buffer);
    }
    if (backend == DISK_NVME) {
        return axys_nvme_read(lba, count, buffer);
    }
    return axys_ata_read(lba, count, buffer);
}

int axys_disk_write(axys_uint32_t lba, axys_uint32_t count, const void *buffer)
{
    if (!range_ok(lba, count)) {
        return -1;
    }
    if (backend == DISK_AHCI) {
        return axys_ahci_write(lba, count, buffer);
    }
    if (backend == DISK_NVME) {
        return axys_nvme_write(lba, count, buffer);
    }
    return axys_ata_write(lba, count, buffer);
}

int axys_disk_flush(void)
{
    if (backend == DISK_AHCI) {
        return axys_ahci_flush();
    }
    if (backend == DISK_NVME) {
        return axys_nvme_flush();
    }
    if (backend == DISK_ATA) {
        return axys_ata_flush();
    }
    return -1;
}
