#ifndef AXYS_VMM_H
#define AXYS_VMM_H

#include "axys/types.h"
#include "axys/multiboot2.h"

/* x86_64 identity-map support. Boot assembly establishes a low 1 GiB map and
 * this module extends the first 4 GiB with static page directories. Above that,
 * it installs sparse identity mappings for firmware-available RAM and explicit
 * reserved PCI MMIO ranges, using page-table pages allocated from the low-memory
 * PMM. The low-canonical layout supports physical addresses through 512 GiB on
 * CPUs whose CPUID width allows it. */

#define AXYS_VMM_MAX_IDENTITY_BYTES 0x100000000ULL /* static bootstrap tables */
#define AXYS_VMM_MAX_PHYSICAL_BYTES 0x8000000000ULL /* 512 GiB, one low PML4 slot */

/* Grow the dense bootstrap identity map so at least `limit_bytes` is mapped,
 * in 1 GiB steps, capped at AXYS_VMM_MAX_IDENTITY_BYTES. Safe to
 * call repeatedly with a growing limit; never unmaps anything, and calling it
 * with a limit at or below what is already mapped is a no-op beyond a TLB
 * flush. Returns the identity limit after the call. */
axys_uint64_t axys_vmm_extend_identity(axys_uint64_t limit_bytes);

/* Identity-map only page-aligned, firmware-available RAM above 4 GiB. Uses
 * 2 MiB pages where possible and 4 KiB pages at range edges. Existing low
 * memory mappings remain unchanged. Returns zero if page-table allocation or
 * CPU physical-address width prevents the requested extension. */
int axys_vmm_map_high_ram(const struct axys_boot_info *info,
                          axys_uint64_t limit_bytes);

/* Highest physical address the current CPU can safely encode in page tables,
 * bounded by this VMM's low-canonical identity-map layout. */
axys_uint64_t axys_vmm_physical_limit(void);

/* Exclusive end of the dense bootstrap identity map (1 GiB immediately after
 * entry, 4 GiB after PMM initialization). Sparse mappings do not change it. */
axys_uint64_t axys_vmm_identity_limit(void);

/* Retain the validated firmware map for later device-range checks. Call after
 * Multiboot2 parsing and before probing PCI devices. */
void axys_vmm_set_memory_map(const struct axys_boot_info *info);

/* Map a firmware-reserved PCI MMIO range at its identity address and mark it
 * uncached (PWT+PCD). Available RAM overlaps, address zero, overflow, and
 * addresses beyond the supported physical limit are rejected. Requires PMM
 * initialization because large pages may need splitting. */
int axys_vmm_map_mmio_range(axys_uint64_t physical, axys_uint64_t length);

#endif
