#include <string.h>
#include "axys/disk.h"

/* A RAM-backed "disk" for host tests, with fault injection. */
#define RAM_SECTORS 4096u
static unsigned char ram[RAM_SECTORS * AXYS_SECTOR_SIZE];
int ramdisk_present = 1;
unsigned ramdisk_fail_writes_after = 0xffffffffu; /* writes allowed before "power loss" */

unsigned char *ramdisk_bytes(void) { return ram; }

int axys_disk_init(void) { return ramdisk_present ? 0 : -1; }
axys_uint32_t axys_disk_sectors(void) { return ramdisk_present ? RAM_SECTORS : 0; }

int axys_disk_read(axys_uint32_t lba, axys_uint32_t count, void *buffer)
{
    if (lba >= RAM_SECTORS || count > RAM_SECTORS - lba) {
        return -1;
    }
    memcpy(buffer, ram + (size_t)lba * AXYS_SECTOR_SIZE, (size_t)count * AXYS_SECTOR_SIZE);
    return 0;
}

int axys_disk_write(axys_uint32_t lba, axys_uint32_t count, const void *buffer)
{
    if (lba >= RAM_SECTORS || count > RAM_SECTORS - lba) {
        return -1;
    }
    for (axys_uint32_t i = 0; i < count; ++i) {
        if (ramdisk_fail_writes_after == 0) {
            return -1; /* power lost: this and every later write vanish */
        }
        --ramdisk_fail_writes_after;
        memcpy(ram + (size_t)(lba + i) * AXYS_SECTOR_SIZE, (const char *)buffer + (size_t)i * AXYS_SECTOR_SIZE,
               AXYS_SECTOR_SIZE);
    }
    return 0;
}

int axys_disk_flush(void) { return 0; }
