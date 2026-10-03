#ifndef AXYS_INITRD_H
#define AXYS_INITRD_H

#include "axys/types.h"

/* Install every embedded file (user programs) into the VFS, creating parent
 * directories as needed. Returns the number of files installed, or -1. */
int axys_initrd_install(void);

/* Create the default /etc files and /home/user unless they already exist. */
int axys_initrd_install_defaults(void);

#endif
