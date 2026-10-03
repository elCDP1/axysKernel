#include "axys/vmm.h"
#include "axys/cpu.h"
#include "axys/pmm.h"
#include "axys/string.h"

#define GIB_BYTES 0x40000000ULL
#define TWO_MIB_BYTES 0x200000ULL
#define PDE_PRESENT_WRITABLE_HUGE_NX 0x8000000000000083ULL /* present | writable | PS | NX */
#define PDPTE_PRESENT_WRITABLE 0x3ULL
#define PDPTE_PRESENT_WRITABLE_NX 0x8000000000000003ULL
#define PTE_PRESENT_WRITABLE_NX 0x8000000000000003ULL
#define PAGE_ADDR_MASK 0x000ffffffffff000ULL
#define HUGE_PAGE_ADDR_MASK 0x000fffffffe00000ULL
#define PAGE_WRITE_THROUGH 0x0000000000000008ULL
#define PAGE_CACHE_DISABLE 0x0000000000000010ULL
#define HUGE_TO_PTE_FLAGS 0x800000000000017fULL

/* Page directories boot.S reserved but did not link into pdpt_table. Each
 * covers exactly 1 GiB with 512 2 MiB entries. All huge identity mappings are
 * supervisor-only and NX; only the first 2 MiB is split into 4 KiB pages so the
 * kernel image can carry distinct text permissions. */
extern axys_uint64_t pdpt_table[512];
extern axys_uint64_t pd_table1[512];
extern axys_uint64_t pd_table2[512];
extern axys_uint64_t pd_table3[512];

static axys_uint64_t *const extra_pd[3] = {pd_table1, pd_table2, pd_table3};
static struct axys_boot_info firmware_map;
static int firmware_map_ready;

/* boot.S already mapped the first 1 GiB before any C code runs. */
static axys_uint64_t identity_limit_bytes = GIB_BYTES;

axys_uint64_t axys_vmm_identity_limit(void)
{
    return identity_limit_bytes;
}

axys_uint64_t axys_vmm_extend_identity(axys_uint64_t limit_bytes)
{
    int extended = 0;

    if (limit_bytes > AXYS_VMM_MAX_IDENTITY_BYTES) {
        limit_bytes = AXYS_VMM_MAX_IDENTITY_BYTES;
    }

    while (identity_limit_bytes < limit_bytes) {
        axys_uint64_t gib_index = identity_limit_bytes / GIB_BYTES; /* 1, 2, or 3 */

        if (gib_index < 1 || gib_index > 3) {
            break; /* out of spare page directories */
        }

        axys_uint64_t *pd = extra_pd[gib_index - 1];
        axys_uint64_t phys_pd = (axys_uint64_t)(axys_uintptr_t)pd;
        axys_uint64_t base = gib_index * GIB_BYTES;

        for (unsigned i = 0; i < 512; ++i) {
            pd[i] = (base + (axys_uint64_t)i * TWO_MIB_BYTES) | PDE_PRESENT_WRITABLE_HUGE_NX;
        }
        pdpt_table[gib_index] = phys_pd | PDPTE_PRESENT_WRITABLE;
        identity_limit_bytes = base + GIB_BYTES;
        extended = 1;
    }

    if (extended) {
        /* New entries were previously not-present; a flush is cheap insurance
         * against a CPU that cached the absence. */
        axys_cpu_write_cr3(axys_cpu_read_cr3());
    }
    return identity_limit_bytes;
}

axys_uint64_t axys_vmm_physical_limit(void)
{
    axys_uint32_t eax = 0;
    axys_uint32_t ebx = 0;
    axys_uint32_t ecx = 0;
    axys_uint32_t edx = 0;
    axys_uint32_t width = 36u; /* architectural PAE minimum */
    axys_uint64_t limit;

    if (axys_cpu_extended_leaf_max() >= 0x80000008u) {
        axys_cpu_cpuid(0x80000008u, 0, &eax, &ebx, &ecx, &edx);
        if ((eax & 0xffu) >= 32u && (eax & 0xffu) <= 52u) {
            width = eax & 0xffu;
        }
    }
    limit = (axys_uint64_t)1u << width;
    if (limit > AXYS_VMM_MAX_PHYSICAL_BYTES) {
        limit = AXYS_VMM_MAX_PHYSICAL_BYTES;
    }
    return limit;
}

/* Return a writable virtual pointer to the page directory for one 1 GiB
 * identity-map slice. The first four directories are part of the boot image;
 * later ones are allocated from the already-initialised low-memory PMM. */
static axys_uint64_t *directory_for_gib(axys_uint64_t gib_index)
{
    axys_uint64_t entry;
    axys_uint64_t frame;

    if (gib_index >= 512u) {
        return AXYS_NULL;
    }
    entry = pdpt_table[gib_index];
    if ((entry & 1u) == 0) {
        frame = axys_pmm_alloc_frame();
        if (frame == 0) {
            return AXYS_NULL;
        }
        axys_memset((void *)(axys_uintptr_t)frame, 0, AXYS_PMM_FRAME_SIZE);
        pdpt_table[gib_index] = frame | PDPTE_PRESENT_WRITABLE_NX;
        entry = pdpt_table[gib_index];
    }
    return (axys_uint64_t *)(axys_uintptr_t)(entry & PAGE_ADDR_MASK);
}

static int map_high_page(axys_uint64_t physical)
{
    axys_uint64_t *pd;
    axys_uint64_t pde_index;
    axys_uint64_t pte_index;
    axys_uint64_t pde;
    axys_uint64_t frame;
    axys_uint64_t *pt;

    pd = directory_for_gib(physical / GIB_BYTES);
    if (pd == AXYS_NULL) {
        return -1;
    }
    pde_index = (physical % GIB_BYTES) / TWO_MIB_BYTES;
    pte_index = (physical % TWO_MIB_BYTES) / AXYS_PMM_FRAME_SIZE;
    pde = pd[pde_index];
    if ((pde & 1u) == 0) {
        frame = axys_pmm_alloc_frame();
        if (frame == 0) {
            return -1;
        }
        axys_memset((void *)(axys_uintptr_t)frame, 0, AXYS_PMM_FRAME_SIZE);
        pd[pde_index] = frame | PDPTE_PRESENT_WRITABLE_NX;
        pde = pd[pde_index];
    } else if ((pde & (1u << 7)) != 0) {
        return 0; /* a previous overlapping map already covers this page */
    }
    pt = (axys_uint64_t *)(axys_uintptr_t)(pde & PAGE_ADDR_MASK);
    pt[pte_index] = physical | PTE_PRESENT_WRITABLE_NX;
    return 0;
}

static int map_high_huge_page(axys_uint64_t physical)
{
    axys_uint64_t *pd = directory_for_gib(physical / GIB_BYTES);
    axys_uint64_t pde_index = (physical % GIB_BYTES) / TWO_MIB_BYTES;
    axys_uint64_t pde;

    if (pd == AXYS_NULL) {
        return -1;
    }
    pde = pd[pde_index];
    if ((pde & 1u) != 0) {
        if ((pde & (1u << 7)) != 0) {
            return 0; /* already mapped by a prior available range */
        }
        /* Preserve any 4 KiB edge pages already installed in this PDE. */
        axys_uint64_t *pt = (axys_uint64_t *)(axys_uintptr_t)(pde & PAGE_ADDR_MASK);
        for (unsigned i = 0; i < 512u; ++i) {
            pt[i] = (physical + (axys_uint64_t)i * AXYS_PMM_FRAME_SIZE) |
                    PTE_PRESENT_WRITABLE_NX;
        }
        return 0;
    }
    pd[pde_index] = physical | PDE_PRESENT_WRITABLE_HUGE_NX;
    return 0;
}

int axys_vmm_map_high_ram(const struct axys_boot_info *info,
                          axys_uint64_t limit_bytes)
{
    axys_uint64_t physical_limit;
    int changed = 0;

    if (info == AXYS_NULL || info->mmap == AXYS_NULL) {
        return -1;
    }
    physical_limit = axys_vmm_physical_limit();
    if (limit_bytes > physical_limit) {
        limit_bytes = physical_limit;
    }
    if (limit_bytes > AXYS_VMM_MAX_PHYSICAL_BYTES) {
        limit_bytes = AXYS_VMM_MAX_PHYSICAL_BYTES;
    }
    if (limit_bytes <= AXYS_VMM_MAX_IDENTITY_BYTES) {
        return 0;
    }

    for (axys_size_t index = 0; index < info->mmap_entries; ++index) {
        const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);
        axys_uint64_t start;
        axys_uint64_t end;

        if (entry == AXYS_NULL || entry->type != AXYS_MMAP_AVAILABLE ||
            entry->length == 0 || entry->address + entry->length < entry->address) {
            continue;
        }
        start = entry->address;
        end = entry->address + entry->length;
        if (start < AXYS_VMM_MAX_IDENTITY_BYTES) {
            start = AXYS_VMM_MAX_IDENTITY_BYTES;
        }
        if (end > limit_bytes) {
            end = limit_bytes;
        }
        if (start > ~(axys_uint64_t)0 - (AXYS_PMM_FRAME_SIZE - 1u)) {
            continue;
        }
        start = (start + AXYS_PMM_FRAME_SIZE - 1u) &
                ~((axys_uint64_t)AXYS_PMM_FRAME_SIZE - 1u);
        end &= ~((axys_uint64_t)AXYS_PMM_FRAME_SIZE - 1u);
        while (start < end) {
            int result;

            if ((start & (TWO_MIB_BYTES - 1u)) == 0 && end - start >= TWO_MIB_BYTES) {
                result = map_high_huge_page(start);
                start += TWO_MIB_BYTES;
            } else {
                result = map_high_page(start);
                start += AXYS_PMM_FRAME_SIZE;
            }
            if (result != 0) {
                if (changed) {
                    axys_cpu_write_cr3(axys_cpu_read_cr3());
                }
                return -1;
            }
            changed = 1;
        }
    }

    if (changed) {
        axys_cpu_write_cr3(axys_cpu_read_cr3());
    }
    return 0;
}

void axys_vmm_set_memory_map(const struct axys_boot_info *info)
{
    firmware_map_ready = 0;
    if (info == AXYS_NULL || info->mmap == AXYS_NULL) {
        return;
    }
    axys_memcpy(&firmware_map, info, sizeof(firmware_map));
    firmware_map_ready = 1;
}

static int overlaps_available_ram(axys_uint64_t start, axys_uint64_t end)
{
    for (axys_size_t i = 0; i < firmware_map.mmap_entries; ++i) {
        const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(&firmware_map, i);
        axys_uint64_t entry_end;

        if (entry == AXYS_NULL || entry->type != AXYS_MMAP_AVAILABLE || entry->length == 0u) {
            continue;
        }
        entry_end = entry->address + entry->length;
        if (entry_end < entry->address) {
            return 1; /* malformed range: refuse to alias it as MMIO */
        }
        if (start < entry_end && entry->address < end) {
            return 1;
        }
    }
    return 0;
}

/* Convert one existing 2 MiB identity mapping into a 4 KiB page table, keeping
 * the effective permissions and cache flags of every page. PS (bit 7) is not
 * copied: in a 4 KiB PTE it is the PAT bit, not the large-page selector. */
static axys_uint64_t *split_huge_page(axys_uint64_t *pd, axys_uint64_t pde_index,
                                     axys_uint64_t pde)
{
    axys_uint64_t frame = axys_pmm_alloc_frame();
    axys_uint64_t *pt;
    axys_uint64_t base = pde & HUGE_PAGE_ADDR_MASK;
    axys_uint64_t flags = pde & HUGE_TO_PTE_FLAGS;

    if (frame == 0u) {
        return AXYS_NULL;
    }
    pt = (axys_uint64_t *)(axys_uintptr_t)frame;
    for (unsigned i = 0; i < 512u; ++i) {
        pt[i] = (base + (axys_uint64_t)i * AXYS_PMM_FRAME_SIZE) | flags;
    }
    pd[pde_index] = frame | PDPTE_PRESENT_WRITABLE_NX;
    return pt;
}

static int map_mmio_page(axys_uint64_t physical)
{
    axys_uint64_t *pd;
    axys_uint64_t pde_index;
    axys_uint64_t pte_index;
    axys_uint64_t pde;
    axys_uint64_t frame;
    axys_uint64_t *pt;
    axys_uint64_t pte;

    if (physical == 0u) {
        return -1; /* preserve the deliberate unmapped null page */
    }
    pd = directory_for_gib(physical / GIB_BYTES);
    if (pd == AXYS_NULL) {
        return -1;
    }
    pde_index = (physical % GIB_BYTES) / TWO_MIB_BYTES;
    pte_index = (physical % TWO_MIB_BYTES) / AXYS_PMM_FRAME_SIZE;
    pde = pd[pde_index];
    if ((pde & 1u) == 0u) {
        frame = axys_pmm_alloc_frame();
        if (frame == 0u) {
            return -1;
        }
        axys_memset((void *)(axys_uintptr_t)frame, 0, AXYS_PMM_FRAME_SIZE);
        pd[pde_index] = frame | PDPTE_PRESENT_WRITABLE_NX;
        pde = pd[pde_index];
    } else if ((pde & (1u << 7)) != 0u) {
        pt = split_huge_page(pd, pde_index, pde);
        if (pt == AXYS_NULL) {
            return -1;
        }
        pde = pd[pde_index];
    }
    pt = (axys_uint64_t *)(axys_uintptr_t)(pde & PAGE_ADDR_MASK);
    pte = pt[pte_index];
    if ((pte & 1u) != 0u) {
        if ((pte & PAGE_ADDR_MASK) != physical) {
            return -1;
        }
        pt[pte_index] = pte | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH | 0x8000000000000000ULL;
    } else {
        pt[pte_index] = physical | PTE_PRESENT_WRITABLE_NX |
                        PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH;
    }
    return 0;
}

int axys_vmm_map_mmio_range(axys_uint64_t physical, axys_uint64_t length)
{
    axys_uint64_t end;
    axys_uint64_t start_page;
    axys_uint64_t end_page;
    axys_uint64_t physical_limit;
    int changed = 0;

    if (!firmware_map_ready || length == 0u || axys_pmm_total_frames() == 0u ||
        physical > ~(axys_uint64_t)0 - length) {
        return -1;
    }
    end = physical + length;
    physical_limit = axys_vmm_physical_limit();
    if (end > physical_limit || end > AXYS_VMM_MAX_PHYSICAL_BYTES ||
        end > ~(axys_uint64_t)0 - (AXYS_PMM_FRAME_SIZE - 1u)) {
        return -1;
    }
    start_page = physical & ~((axys_uint64_t)AXYS_PMM_FRAME_SIZE - 1u);
    end_page = (end + AXYS_PMM_FRAME_SIZE - 1u) &
               ~((axys_uint64_t)AXYS_PMM_FRAME_SIZE - 1u);
    if (overlaps_available_ram(start_page, end_page)) {
        return -1;
    }

    for (axys_uint64_t page = start_page; page < end_page;
         page += AXYS_PMM_FRAME_SIZE) {
        if (map_mmio_page(page) != 0) {
            axys_cpu_write_cr3(axys_cpu_read_cr3());
            return -1;
        }
        changed = 1;
    }
    if (changed) {
        axys_cpu_write_cr3(axys_cpu_read_cr3());
    }
    return 0;
}
