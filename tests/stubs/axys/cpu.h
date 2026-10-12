#ifndef AXYS_STUB_CPU_H
#define AXYS_STUB_CPU_H

#include "axys/types.h"

/* Single-threaded host: interrupt masking is a no-op. */
static inline axys_uint64_t axys_cpu_save_flags(void)
{
    return 0;
}

static inline void axys_cpu_restore_flags(axys_uint64_t flags)
{
    (void)flags;
}

static inline void axys_cpu_disable_interrupts(void)
{
}

#endif
