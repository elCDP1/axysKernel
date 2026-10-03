#ifndef AXYS_ASPACE_H
#define AXYS_ASPACE_H

#include "axys/types.h"

/*
 * Per-process address spaces.
 *
 * Every address space is a private PML4. Entry 0 is copied from the kernel's
 * PML4, so the identity-mapped kernel (supervisor-only pages, U/S = 0) is
 * present in every space and is unreachable from ring 3. Entry 1 -- the 512 GiB
 * window [AXYS_USER_BASE, AXYS_USER_TOP) -- is the only part user code can
 * touch, and it belongs to the process alone.
 *
 * Kernel code never dereferences user virtual addresses directly: it goes
 * through axys_aspace_copy_from()/copy_to(), which walk the tables in
 * software and copy via the kernel's identity map. A bad user pointer
 * therefore yields an error, never a kernel page fault.
 */

#define AXYS_USER_BASE 0x0000008000000000ULL /* PML4 index 1 */
#define AXYS_USER_TOP 0x0000010000000000ULL

#define AXYS_MAP_WRITE (1u << 0)
#define AXYS_MAP_EXEC (1u << 1)

struct axys_aspace {
    axys_uint64_t pml4; /* physical address of the PML4 */
};

/* Create an empty user address space. Returns 0 or -1 (out of memory). */
int axys_aspace_create(struct axys_aspace *space);

/* Free every user page and every table of the space (never the kernel's). */
void axys_aspace_destroy(struct axys_aspace *space);

/* Back [virt, virt+length) with fresh zeroed frames. Both must be
 * page-aligned and inside the user window. Mapping W and X together is
 * refused (W^X). Returns 0 or -1; on failure nothing new remains mapped. */
int axys_aspace_map_zeroed(struct axys_aspace *space, axys_uint64_t virt,
                           axys_uint64_t length, axys_uint32_t flags);

/* Remove the mapping for [virt, virt+length) and free its frames. */
void axys_aspace_unmap(struct axys_aspace *space, axys_uint64_t virt, axys_uint64_t length);

/* Safe user-memory access. Return 0 on success, -1 if any byte of the range is
 * outside the user window or not mapped (or not writable for copy_to). */
int axys_aspace_copy_from(const struct axys_aspace *space, void *kernel_dst,
                          axys_uint64_t user_src, axys_size_t length);
int axys_aspace_copy_to(const struct axys_aspace *space, axys_uint64_t user_dst,
                        const void *kernel_src, axys_size_t length);

/* Verify that [user, user+length) is mapped (and writable if `write`) without
 * touching it. Lets a blocking syscall fail with EFAULT *before* it sleeps. */
int axys_aspace_check(const struct axys_aspace *space, axys_uint64_t user, axys_size_t length, int write);

/* Kernel-initiated write used by the program loader: like copy_to but ignores
 * the page's W bit (code segments are mapped read-only/executable). */
int axys_aspace_load(const struct axys_aspace *space, axys_uint64_t user_dst,
                     const void *kernel_src, axys_size_t length);

/* Copy a NUL-terminated string from user space into kernel_dst (max bytes,
 * including the terminator). Returns the length or -1 on fault/overlong. */
axys_int32_t axys_aspace_copy_string(const struct axys_aspace *space, char *kernel_dst,
                                     axys_uint64_t user_src, axys_size_t max);

/* Is the hardware NX bit active? (Otherwise W^X is enforced only at load.) */
int axys_aspace_nx_active(void);

#endif
