#ifndef AXYS_PERSIST_H
#define AXYS_PERSIST_H

#include "axys/types.h"

/*
 * Persistent storage for the in-memory file system.
 *
 * The VFS lives in RAM. This layer saves the user-data part of it to the system
 * disk as an atomic snapshot and restores it at boot:
 *
 *   persisted:     /etc /home /root /var /srv /opt /usr /mnt /media
 *   never saved:   /bin /sbin (they come from the kernel image), /tmp, /run,
 *                  /dev, /proc, /sys, /boot, /lib
 *
 * Disk layout: sector 0 superblock; then two snapshot slots (A/B). A sync
 * writes the *inactive* slot (payload, flush, then header, flush), so a power
 * cut at any point leaves the previous snapshot intact. Mounting picks the
 * valid slot with the highest generation. Everything is CRC-32 protected, and
 * restore only accepts paths inside the persisted roots, so a tampered disk
 * cannot replace system programs.
 */

/* Probe the disk and restore the last snapshot. Returns:
 *   >= 0  number of files restored (disk present, possibly freshly formatted)
 *   -1    no usable disk (the system runs volatile)
 *   -2    the disk holds foreign data; it is left untouched */
int axys_persist_init(void);

/* Write a snapshot now. Returns 0, or -1 on error / no disk. Safe to call from
 * any task; concurrent calls are serialised. */
int axys_persist_sync(void);

int axys_persist_available(void);
axys_uint64_t axys_persist_generation(void);

/* Start the background task that syncs changes every few seconds. */
void axys_persist_start_flusher(void);

/* CRC-32 (IEEE 802.3). Exposed for tests. */
axys_uint32_t axys_crc32(axys_uint32_t crc, const void *data, axys_size_t length);

#endif
