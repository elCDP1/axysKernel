#ifndef AXYS_PMM_H
#define AXYS_PMM_H

#include "axys/multiboot2.h"
#include "axys/types.h"
#include "axys/vmm.h"

/* Physical page-frame allocator. The static bootstrap maps and bitmaps cover
 * the first 4 GiB. axys_pmm_extend() maps additional firmware-reported RAM,
 * grows the bitmaps using low memory, then admits only mapped, available RAM
 * up to the architecture's direct-map ceiling. */

#define AXYS_PMM_FRAME_SIZE 4096u
#define AXYS_PMM_INITIAL_LIMIT_BYTES 0x100000000ULL
#define AXYS_PMM_INITIAL_FRAMES (AXYS_PMM_INITIAL_LIMIT_BYTES / AXYS_PMM_FRAME_SIZE)

/* Build the bitmap from the firmware memory map: everything not explicitly
 * marked AXYS_MMAP_AVAILABLE starts reserved, and the low 1 MiB (legacy BIOS
 * data), the kernel's own image, and the Multiboot2 info block are always
 * reserved on top of that regardless of what the map claims. Extends the
 * identity map (axys_vmm_extend_identity) as needed first. Must run after
 * axys_multiboot2_parse() and before any allocation call. */
void axys_pmm_init(const struct axys_boot_info *info);

/* Extend the managed physical address range above 4 GiB when the firmware
 * memory map contains usable RAM there. Returns zero on success (including
 * when no high RAM is present), nonzero if mapping or metadata growth fails;
 * the original low-memory allocator remains usable on failure. */
int axys_pmm_extend(const struct axys_boot_info *info);

/* Hand back one free 4 KiB frame, marking it used, or 0 if none remain.
 * Physical address 0 is always reserved (part of the low-1MiB carve-out), so
 * 0 is an unambiguous failure value. */
axys_uint64_t axys_pmm_alloc_frame(void);

/* Return a frame obtained from axys_pmm_alloc_frame() to the free pool.
 * Freeing an address that is not a frame-aligned, in-range, currently-used
 * frame is a no-op. */
void axys_pmm_free_frame(axys_uint64_t address);

/* Allocate `count` physically contiguous frames (task stacks, DMA buffers,
 * large heap blocks). Returns the physical address of the first frame or 0.
 * Release with axys_pmm_free_contiguous() using the same count. */
axys_uint64_t axys_pmm_alloc_contiguous(axys_uint64_t count);
void axys_pmm_free_contiguous(axys_uint64_t address, axys_uint64_t count);

axys_uint64_t axys_pmm_total_frames(void);
axys_uint64_t axys_pmm_free_frames(void);
axys_uint64_t axys_pmm_used_frames(void);

/* Exclusive physical-address ceiling tracked by the allocator. Firmware
 * holes below this ceiling remain reserved and are not handed out. */
axys_uint64_t axys_pmm_identity_limit(void);

/* Allocates, frees, double-frees and exhausts a handful of frames to prove
 * the bitmap logic is sound. Panics on the first mismatch. */
void axys_pmm_selftest(void);

#endif
