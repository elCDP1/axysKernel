#ifndef AXYS_KERNEL_H
#define AXYS_KERNEL_H

#include "axys/types.h"

void axys_kmain(axys_uint32_t multiboot_magic, axys_uint32_t multiboot_information);

/* Kernel release shown by `uname` (via /proc/version). Bump on releases. */
#define AXYS_VERSION "1.0"

#endif
