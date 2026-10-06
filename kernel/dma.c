#include "axys/dma.h"
#include "axys/pmm.h"
#include "axys/spinlock.h"
#include "axys/string.h"

/* DMA-able memory, backed by whole PMM frames. The table below is the only
 * state: who owns which run, so dma_free() releases exactly what dma_alloc()
 * took. Bounded and static (64 live allocations); beyond that allocation
 * fails cleanly instead of growing bookkeeping without limit. */

#define DMA_MAX_LIVE 64u
#define DMA_FRAME_BYTES AXYS_PMM_FRAME_SIZE
#define DMA_4GB (1ull << 32)

struct dma_entry {
    int used;
    axys_uint64_t base;   /* run start (what the PMM handed out) */
    axys_uint64_t frames; /* run length */
};

static struct dma_entry table[DMA_MAX_LIVE];
static struct axys_spinlock dma_lock;

static int is_pow2(axys_size_t v)
{
    return v != 0 && (v & (v - 1u)) == 0;
}

static axys_uint64_t align_up_u64(axys_uint64_t v, axys_uint64_t a)
{
    axys_uint64_t mask = a - 1u;

    if (v > ~(axys_uint64_t)0 - mask) {
        return 0; /* overflow: no aligned address exists */
    }
    return (v + mask) & ~mask;
}

void *axys_dma_alloc(axys_size_t size, axys_size_t alignment, axys_size_t boundary,
                     axys_uint32_t flags, axys_uint64_t *phys)
{
    axys_uint64_t extra;
    axys_uint64_t total;
    axys_uint64_t frames;
    axys_uint64_t base;
    axys_uint64_t picked = 0;
    axys_uint64_t locked;
    int slot = -1;

    if (phys == AXYS_NULL || size == 0) {
        return AXYS_NULL;
    }
    if (alignment == 0) {
        alignment = 1;
    }
    if (!is_pow2(alignment) || (boundary != 0 && !is_pow2(boundary))) {
        return AXYS_NULL;
    }
    if (boundary != 0 && (axys_uint64_t)size > boundary) {
        /* No placement inside one boundary block can hold the buffer. */
        return AXYS_NULL;
    }
    /* Worst case we need the buffer plus one alignment of slack plus one
     * boundary of slack to slide into a fitting window. */
    extra = (axys_uint64_t)alignment + (axys_uint64_t)boundary;
    if (extra < (axys_uint64_t)alignment || extra < (axys_uint64_t)boundary) {
        return AXYS_NULL; /* wrapped */
    }
    total = (axys_uint64_t)size + extra;
    if (total < (axys_uint64_t)size) {
        return AXYS_NULL;
    }
    frames = (total + DMA_FRAME_BYTES - 1u) / DMA_FRAME_BYTES;
    if (frames == 0) {
        return AXYS_NULL;
    }
    base = axys_pmm_alloc_contiguous(frames);
    if (base == 0) {
        return AXYS_NULL;
    }
    picked = align_up_u64(base, alignment);
    if (picked == 0 || picked < base || picked + (axys_uint64_t)size < picked ||
        picked + (axys_uint64_t)size > base + frames * DMA_FRAME_BYTES) {
        axys_pmm_free_contiguous(base, frames);
        return AXYS_NULL;
    }
    if (boundary != 0) {
        /* Slide forward until the whole buffer sits in one boundary block. A
         * fit always exists: the run is longer than size + boundary, so some
         * boundary block inside it holds the buffer. */
        for (unsigned tries = 0; tries < 1024u; ++tries) {
            axys_uint64_t end = picked + (axys_uint64_t)size - 1u;

            if (picked / boundary == end / boundary) {
                break;
            }
            picked = align_up_u64(((picked / boundary) + 1u) * boundary, alignment);
            if (picked == 0 || picked + (axys_uint64_t)size < picked ||
                picked + (axys_uint64_t)size > base + frames * DMA_FRAME_BYTES) {
                axys_pmm_free_contiguous(base, frames);
                return AXYS_NULL;
            }
        }
        if (picked / boundary != (picked + (axys_uint64_t)size - 1u) / boundary) {
            axys_pmm_free_contiguous(base, frames);
            return AXYS_NULL;
        }
    }
    if ((flags & AXYS_DMA_32BIT) != 0u && picked + (axys_uint64_t)size > DMA_4GB) {
        axys_pmm_free_contiguous(base, frames);
        return AXYS_NULL;
    }
    locked = axys_spin_lock_irqsave(&dma_lock);
    for (unsigned i = 0; i < DMA_MAX_LIVE; ++i) {
        if (!table[i].used) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        axys_spin_unlock_irqrestore(&dma_lock, locked);
        axys_pmm_free_contiguous(base, frames);
        return AXYS_NULL;
    }
    table[slot].used = 1;
    table[slot].base = base;
    table[slot].frames = frames;
    axys_spin_unlock_irqrestore(&dma_lock, locked);

    axys_memset((void *)(axys_uintptr_t)picked, 0, size);
    *phys = picked; /* identity map: virtual address == DMA address */
    return (void *)(axys_uintptr_t)picked;
}

void axys_dma_free(void *buffer)
{
    axys_uint64_t locked;

    if (buffer == AXYS_NULL) {
        return;
    }
    locked = axys_spin_lock_irqsave(&dma_lock);
    for (unsigned i = 0; i < DMA_MAX_LIVE; ++i) {
        if (table[i].used && table[i].base <= (axys_uint64_t)(axys_uintptr_t)buffer &&
            (axys_uint64_t)(axys_uintptr_t)buffer <
                table[i].base + table[i].frames * DMA_FRAME_BYTES) {
            axys_uint64_t base = table[i].base;
            axys_uint64_t frames = table[i].frames;

            table[i].used = 0;
            table[i].base = 0;
            table[i].frames = 0;
            axys_spin_unlock_irqrestore(&dma_lock, locked);
            axys_pmm_free_contiguous(base, frames);
            return;
        }
    }
    axys_spin_unlock_irqrestore(&dma_lock, locked);
}

axys_uint32_t axys_dma_live_allocations(void)
{
    axys_uint64_t locked = axys_spin_lock_irqsave(&dma_lock);
    axys_uint32_t live = 0;

    for (unsigned i = 0; i < DMA_MAX_LIVE; ++i) {
        if (table[i].used) {
            ++live;
        }
    }
    axys_spin_unlock_irqrestore(&dma_lock, locked);
    return live;
}
