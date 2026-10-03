#include "axys/aspace.h"
#include "axys/cpu.h"
#include "axys/pmm.h"
#include "axys/string.h"

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE (1ull << 1)
#define PTE_USER (1ull << 2)
#define PTE_NX (1ull << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL
#define PAGE 4096ULL

extern axys_uint64_t pml4_table[512];

static axys_uint64_t *table_at(axys_uint64_t phys)
{
    return (axys_uint64_t *)(axys_uintptr_t)phys; /* identity-mapped kernel */
}

int axys_aspace_nx_active(void)
{
    return (axys_cpu_read_msr(AXYS_MSR_EFER) & AXYS_EFER_NXE) != 0;
}

static axys_uint64_t frame_zeroed(void)
{
    axys_uint64_t frame = axys_pmm_alloc_frame();

    if (frame != 0) {
        axys_memset(table_at(frame), 0, PAGE);
    }
    return frame;
}

int axys_aspace_create(struct axys_aspace *space)
{
    axys_uint64_t pml4 = frame_zeroed();

    if (pml4 == 0) {
        return -1;
    }
    table_at(pml4)[0] = pml4_table[0]; /* shared, supervisor-only kernel map */
    space->pml4 = pml4;
    return 0;
}

static int in_user_window(axys_uint64_t virt, axys_uint64_t length)
{
    return virt >= AXYS_USER_BASE && virt < AXYS_USER_TOP && length <= AXYS_USER_TOP - virt;
}

/* Walk to the leaf PTE. With `create`, missing tables are allocated. Returns
 * a pointer to the PTE, or NULL. */
static axys_uint64_t *walk(const struct axys_aspace *space, axys_uint64_t virt, int create)
{
    axys_uint64_t *table = table_at(space->pml4);

    for (int level = 3; level > 0; --level) {
        axys_uint64_t index = (virt >> (12 + 9 * level)) & 511u;
        axys_uint64_t entry = table[index];

        if (!(entry & PTE_PRESENT)) {
            axys_uint64_t frame;

            if (!create) {
                return AXYS_NULL;
            }
            frame = frame_zeroed();
            if (frame == 0) {
                return AXYS_NULL;
            }
            /* Intermediate entries are permissive; the leaf decides. */
            table[index] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
            entry = table[index];
        }
        table = table_at(entry & PTE_ADDR_MASK);
    }
    return &table[(virt >> 12) & 511u];
}

int axys_aspace_map_zeroed(struct axys_aspace *space, axys_uint64_t virt,
                           axys_uint64_t length, axys_uint32_t flags)
{
    axys_uint64_t done;

    if ((virt & (PAGE - 1)) || (length & (PAGE - 1)) || length == 0 ||
        !in_user_window(virt, length)) {
        return -1;
    }
    if ((flags & AXYS_MAP_WRITE) && (flags & AXYS_MAP_EXEC)) {
        return -1; /* W^X */
    }
    for (done = 0; done < length; done += PAGE) {
        axys_uint64_t *pte = walk(space, virt + done, 1);
        axys_uint64_t frame;
        axys_uint64_t value;

        if (pte == AXYS_NULL || (*pte & PTE_PRESENT)) {
            goto fail;
        }
        frame = frame_zeroed();
        if (frame == 0) {
            goto fail;
        }
        value = frame | PTE_PRESENT | PTE_USER;
        if (flags & AXYS_MAP_WRITE) {
            value |= PTE_WRITE;
        }
        if (!(flags & AXYS_MAP_EXEC) && axys_aspace_nx_active()) {
            value |= PTE_NX;
        }
        *pte = value;
    }
    return 0;
fail:
    axys_aspace_unmap(space, virt, done);
    return -1;
}

void axys_aspace_unmap(struct axys_aspace *space, axys_uint64_t virt, axys_uint64_t length)
{
    for (axys_uint64_t off = 0; off < length; off += PAGE) {
        axys_uint64_t *pte = walk(space, virt + off, 0);

        if (pte != AXYS_NULL && (*pte & PTE_PRESENT)) {
            axys_pmm_free_frame(*pte & PTE_ADDR_MASK);
            *pte = 0;
        }
    }
    /* Any TLB entries for the range may linger if this space is active. */
    axys_cpu_write_cr3(axys_cpu_read_cr3());
}

static void free_subtree(axys_uint64_t table_phys, int level)
{
    axys_uint64_t *table = table_at(table_phys);

    for (unsigned i = 0; i < 512; ++i) {
        axys_uint64_t entry = table[i];

        if (!(entry & PTE_PRESENT)) {
            continue;
        }
        if (level > 1) {
            free_subtree(entry & PTE_ADDR_MASK, level - 1);
        } else {
            axys_pmm_free_frame(entry & PTE_ADDR_MASK);
        }
    }
    axys_pmm_free_frame(table_phys);
}

void axys_aspace_destroy(struct axys_aspace *space)
{
    axys_uint64_t *pml4;

    if (space->pml4 == 0) {
        return;
    }
    pml4 = table_at(space->pml4);
    for (unsigned i = 1; i < 512; ++i) { /* entry 0 is the shared kernel map */
        if (pml4[i] & PTE_PRESENT) {
            free_subtree(pml4[i] & PTE_ADDR_MASK, 3);
        }
    }
    axys_pmm_free_frame(space->pml4);
    space->pml4 = 0;
}

/* Copy page by page through the identity map after checking each PTE. */
static int copy_range_ex(const struct axys_aspace *space, axys_uint64_t user, void *kernel,
                         axys_size_t length, int to_user, int force)
{
    axys_uint8_t *k = (axys_uint8_t *)kernel;

    if (length == 0) {
        return 0;
    }
    if (!in_user_window(user, length)) {
        return -1;
    }
    while (length != 0) {
        axys_uint64_t *pte = walk(space, user, 0);
        axys_uint64_t in_page = PAGE - (user & (PAGE - 1));
        axys_size_t chunk = length < in_page ? length : (axys_size_t)in_page;
        axys_uint8_t *phys;

        if (pte == AXYS_NULL || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER) ||
            (to_user && !force && !(*pte & PTE_WRITE))) {
            return -1;
        }
        phys = (axys_uint8_t *)(axys_uintptr_t)((*pte & PTE_ADDR_MASK) + (user & (PAGE - 1)));
        if (to_user) {
            axys_memcpy(phys, k, chunk);
        } else {
            axys_memcpy(k, phys, chunk);
        }
        user += chunk;
        k += chunk;
        length -= chunk;
    }
    return 0;
}

static int copy_range(const struct axys_aspace *space, axys_uint64_t user, void *kernel,
                      axys_size_t length, int to_user)
{
    return copy_range_ex(space, user, kernel, length, to_user, 0);
}

int axys_aspace_check(const struct axys_aspace *space, axys_uint64_t user, axys_size_t length, int write)
{
    if (length == 0) {
        return 0;
    }
    if (!in_user_window(user, length)) {
        return -1;
    }
    for (axys_uint64_t page = user & ~(PAGE - 1); page < user + length; page += PAGE) {
        axys_uint64_t *pte = walk(space, page, 0);

        if (pte == AXYS_NULL || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER) ||
            (write && !(*pte & PTE_WRITE))) {
            return -1;
        }
    }
    return 0;
}

int axys_aspace_load(const struct axys_aspace *space, axys_uint64_t user_dst,
                     const void *kernel_src, axys_size_t length)
{
    return copy_range_ex(space, user_dst, (void *)(axys_uintptr_t)kernel_src, length, 1, 1);
}

int axys_aspace_copy_from(const struct axys_aspace *space, void *kernel_dst,
                          axys_uint64_t user_src, axys_size_t length)
{
    return copy_range(space, user_src, kernel_dst, length, 0);
}

int axys_aspace_copy_to(const struct axys_aspace *space, axys_uint64_t user_dst,
                        const void *kernel_src, axys_size_t length)
{
    return copy_range(space, user_dst, (void *)(axys_uintptr_t)kernel_src, length, 1);
}

axys_int32_t axys_aspace_copy_string(const struct axys_aspace *space, char *kernel_dst,
                                     axys_uint64_t user_src, axys_size_t max)
{
    if (max == 0) {
        return -1; /* no room even for the terminator; never index max - 1 */
    }
    for (axys_size_t i = 0; i < max; ++i) {
        if (copy_range(space, user_src + i, &kernel_dst[i], 1, 0) != 0) {
            return -1;
        }
        if (kernel_dst[i] == '\0') {
            return (axys_int32_t)i;
        }
    }
    kernel_dst[max - 1] = '\0';
    return -1; /* unterminated within the limit */
}
