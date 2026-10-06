#ifndef AXYS_DMA_H
#define AXYS_DMA_H

#include "axys/types.h"

/*
 * DMA-able memory for device drivers (descriptor rings, command lists, data
 * buffers). The kernel identity-maps RAM, so the pointer a driver uses and the
 * physical address it programs into the device are the same number; they are
 * still returned separately so drivers say which one they mean.
 *
 * Memory is zeroed, physically contiguous and aligned as requested. There is no
 * IOMMU: a device can DMA anywhere, so only hand these buffers to hardware you
 * trust. Cache-coherent DMA (normal x86 behaviour) is assumed.
 */

#define AXYS_DMA_32BIT 0x1u /* the device can only address the low 4 GiB */

/*
 * Allocate `size` bytes aligned to `alignment` (a power of two, at least 1) that
 * do not cross a `boundary`-byte boundary (a power of two, or 0 for none). xHCI
 * rings, for example, must not cross a 64 KiB boundary. Returns the buffer (or
 * AXYS_NULL) and stores its physical address in *phys.
 */
void *axys_dma_alloc(axys_size_t size, axys_size_t alignment, axys_size_t boundary,
                     axys_uint32_t flags, axys_uint64_t *phys);

/* Release a buffer returned by axys_dma_alloc(). Unknown pointers are ignored. */
void axys_dma_free(void *buffer);

/* Live allocations and the frames they hold (leak accounting for tests). */
axys_uint32_t axys_dma_live_allocations(void);

#endif
