#ifndef AXYS_HEAP_H
#define AXYS_HEAP_H

#include "axys/types.h"

/*
 * Kernel heap: a first-fit free-list allocator over pages borrowed from the
 * physical allocator (axys/pmm.h). Physical memory is identity-mapped (see
 * axys/vmm.h), so a frame's physical address is also a valid pointer -- no
 * separate virtual-address bookkeeping is needed yet.
 *
 * The heap grows on demand, one arena of AXYS_HEAP_ARENA_FRAMES contiguous
 * frames at a time (falling back to a single frame if that many contiguous
 * frames aren't available), and never shrinks: freed memory goes back onto
 * the free list, not back to the PMM. That keeps the allocator small and
 * predictable, which matters more than peak memory efficiency this early.
 */

/* Must be called once, after axys_pmm_init(), and before any kmalloc. */
void axys_heap_init(void);

void *axys_kmalloc(axys_size_t size);
void axys_kfree(void *pointer);

/* Zeroed allocation. Sizes above one heap arena are served straight from
 * contiguous physical frames, so there is no small fixed cap. */
void *axys_kzalloc(axys_size_t size);

axys_size_t axys_heap_bytes_in_use(void);
axys_size_t axys_heap_bytes_free(void);

/* Exercises alloc/free/coalesce/growth. Panics on the first mismatch. */
void axys_heap_selftest(void);

#endif
