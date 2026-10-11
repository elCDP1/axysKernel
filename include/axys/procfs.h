#ifndef AXYS_PROCFS_H
#define AXYS_PROCFS_H

#include "axys/types.h"

/*
 * Read-only files under /proc generated from live kernel state. There is no
 * dynamic-file support in the VFS, so each file is a normal RAM file that is
 * regenerated just before it is opened: `cat /proc/usb` after unplugging a
 * keyboard, or `ip` after DHCP, shows the current state, not the boot snapshot.
 * /proc never reaches the disk.
 */

/* Create every /proc file (call once the drivers have initialised). */
void axys_procfs_init(void);

/* If `path` is a /proc file this kernel generates, regenerate it now. Any other
 * path is ignored. Safe to call from process context. */
void axys_procfs_refresh(const char *path);

#endif
