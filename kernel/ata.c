#include "axys/ata.h"
#include "axys/disk.h" /* AXYS_SECTOR_SIZE */
#include "axys/io.h"
#include "axys/sched.h"
#include "axys/spinlock.h"
#include "axys/string.h"

/* ATA PIO driver for the primary channel, master drive. Polling only (the
 * device interrupt is masked with nIEN): a disk that never answers produces a
 * timeout and an error, never a hang. */

#define ATA_DATA 0x1f0
#define ATA_ERROR 0x1f1
#define ATA_COUNT 0x1f2
#define ATA_LBA_LO 0x1f3
#define ATA_LBA_MID 0x1f4
#define ATA_LBA_HI 0x1f5
#define ATA_DRIVE 0x1f6
#define ATA_STATUS 0x1f7
#define ATA_COMMAND 0x1f7
#define ATA_CONTROL 0x3f6

#define ST_ERR 0x01u
#define ST_DRQ 0x08u
#define ST_DF 0x20u
#define ST_BSY 0x80u

#define CMD_READ 0x20u
#define CMD_WRITE 0x30u
#define CMD_FLUSH 0xe7u
#define CMD_IDENTIFY 0xecu

#define POLL_LIMIT 2000000u
#define LBA28_MAX 0x0fffffffu

static axys_uint32_t total_sectors;
static int disk_busy;
static struct axys_spinlock disk_lock;

static void delay_400ns(void)
{
    for (int i = 0; i < 4; ++i) {
        (void)axys_inb(ATA_CONTROL);
    }
}

/* Wait for BSY to clear. Returns the final status, or -1 on timeout. */
static int wait_not_busy(void)
{
    for (axys_uint32_t i = 0; i < POLL_LIMIT; ++i) {
        axys_uint8_t status = axys_inb(ATA_STATUS);

        if ((status & ST_BSY) == 0) {
            return status;
        }
    }
    return -1;
}

static int wait_drq(void)
{
    for (axys_uint32_t i = 0; i < POLL_LIMIT; ++i) {
        axys_uint8_t status = axys_inb(ATA_STATUS);

        if (status & (ST_ERR | ST_DF)) {
            return -1;
        }
        if ((status & ST_BSY) == 0 && (status & ST_DRQ) != 0) {
            return 0;
        }
    }
    return -1;
}

static void select_sector(axys_uint32_t lba, axys_uint32_t count)
{
    axys_outb(ATA_DRIVE, (axys_uint8_t)(0xe0u | ((lba >> 24) & 0x0fu))); /* LBA mode, master */
    axys_outb(ATA_COUNT, (axys_uint8_t)count);
    axys_outb(ATA_LBA_LO, (axys_uint8_t)lba);
    axys_outb(ATA_LBA_MID, (axys_uint8_t)(lba >> 8));
    axys_outb(ATA_LBA_HI, (axys_uint8_t)(lba >> 16));
}

static void lock(void)
{
    for (;;) {
        axys_uint64_t flags = axys_spin_lock_irqsave(&disk_lock);

        if (!disk_busy) {
            disk_busy = 1;
            axys_spin_unlock_irqrestore(&disk_lock, flags);
            return;
        }
        axys_spin_unlock_irqrestore(&disk_lock, flags);
        axys_yield();
    }
}

static void unlock(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&disk_lock);

    disk_busy = 0;
    axys_spin_unlock_irqrestore(&disk_lock, flags);
}

int axys_ata_init(void)
{
    axys_uint16_t id[256];
    axys_uint8_t status;

    total_sectors = 0;
    if (axys_inb(ATA_STATUS) == 0xffu) {
        return -1; /* floating bus: no controller/drive */
    }
    axys_outb(ATA_CONTROL, 0x02); /* nIEN: polling only */
    axys_outb(ATA_DRIVE, 0xa0);   /* master */
    delay_400ns();
    axys_outb(ATA_COUNT, 0);
    axys_outb(ATA_LBA_LO, 0);
    axys_outb(ATA_LBA_MID, 0);
    axys_outb(ATA_LBA_HI, 0);
    axys_outb(ATA_COMMAND, CMD_IDENTIFY);
    delay_400ns();
    if (axys_inb(ATA_STATUS) == 0) {
        return -1; /* no drive */
    }
    if (wait_not_busy() < 0) {
        return -1;
    }
    if (axys_inb(ATA_LBA_MID) != 0 || axys_inb(ATA_LBA_HI) != 0) {
        return -1; /* ATAPI / SATA signature: not a plain ATA disk */
    }
    if (wait_drq() != 0) {
        return -1;
    }
    axys_insw(ATA_DATA, id, 256);
    status = axys_inb(ATA_STATUS);
    (void)status;
    total_sectors = ((axys_uint32_t)id[61] << 16) | id[60]; /* LBA28 capacity */
    if (total_sectors > LBA28_MAX) {
        total_sectors = LBA28_MAX;
    }
    return total_sectors != 0 ? 0 : -1;
}

axys_uint32_t axys_ata_sectors(void)
{
    return total_sectors;
}

int axys_ata_read(axys_uint32_t lba, axys_uint32_t count, void *buffer)
{
    axys_uint8_t *out = (axys_uint8_t *)buffer;
    int rc = 0;

    if (total_sectors == 0 || count == 0 || lba >= total_sectors || count > total_sectors - lba) {
        return -1;
    }
    lock();
    for (axys_uint32_t i = 0; i < count && rc == 0; ++i) {
        if (wait_not_busy() < 0) {
            rc = -1;
            break;
        }
        select_sector(lba + i, 1);
        axys_outb(ATA_COMMAND, CMD_READ);
        delay_400ns();
        if (wait_drq() != 0) {
            rc = -1;
            break;
        }
        axys_insw(ATA_DATA, out + (axys_size_t)i * AXYS_SECTOR_SIZE, 256);
    }
    unlock();
    return rc;
}

int axys_ata_write(axys_uint32_t lba, axys_uint32_t count, const void *buffer)
{
    const axys_uint8_t *in = (const axys_uint8_t *)buffer;
    int rc = 0;

    if (total_sectors == 0 || count == 0 || lba >= total_sectors || count > total_sectors - lba) {
        return -1;
    }
    lock();
    for (axys_uint32_t i = 0; i < count && rc == 0; ++i) {
        if (wait_not_busy() < 0) {
            rc = -1;
            break;
        }
        select_sector(lba + i, 1);
        axys_outb(ATA_COMMAND, CMD_WRITE);
        delay_400ns();
        if (wait_drq() != 0) {
            rc = -1;
            break;
        }
        axys_outsw(ATA_DATA, in + (axys_size_t)i * AXYS_SECTOR_SIZE, 256);
        if (wait_not_busy() < 0) {
            rc = -1;
        }
    }
    unlock();
    return rc;
}

int axys_ata_flush(void)
{
    int rc = 0;

    if (total_sectors == 0) {
        return -1;
    }
    lock();
    axys_outb(ATA_DRIVE, 0xe0);
    axys_outb(ATA_COMMAND, CMD_FLUSH);
    if (wait_not_busy() < 0) {
        rc = -1;
    }
    unlock();
    return rc;
}
