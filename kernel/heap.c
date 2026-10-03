#include "axys/heap.h"
#include "axys/panic.h"
#include "axys/pmm.h"
#include "axys/printf.h"
#include "axys/spinlock.h"
#include "axys/string.h"

/* The free list is shared state: once the timer (or any device driver) is
 * live, an interrupt handler that allocates can re-enter kmalloc/kfree in the
 * middle of a traversal and corrupt the singly-linked chain. Every public
 * entry point below therefore runs inside one interrupt-safe critical
 * section. The lock word sits in .data pre-initialised to "unlocked", so it
 * is valid from the first instruction even before axys_heap_init() runs. */
static struct axys_spinlock heap_lock = AXYS_SPINLOCK_INIT(heap_lock);

#define HEAP_ALIGN 16u
#define HEAP_ARENA_FRAMES 4u        /* 16 KiB per growth step */
#define HEAP_MAX_CONTIGUOUS_FRAMES 4u
#define HEAP_MIN_SPLIT_PAYLOAD 32u  /* below this, don't bother splitting off a remainder */

struct block {
    axys_size_t size; /* payload bytes available after this header */
    int free;
    struct block *next; /* next block in ascending-address order */
    axys_uint64_t reserved; /* keeps the header 32 bytes / 16-byte aligned */
};

AXYS_STATIC_ASSERT(sizeof(struct block) == 32u, heap_header_must_be_32_bytes);
#define HEAP_MAX_PAYLOAD \
    (HEAP_MAX_CONTIGUOUS_FRAMES * AXYS_PMM_FRAME_SIZE - sizeof(struct block))

/* Allocations larger than one arena bypass the free list and take whole
 * contiguous frames. A 32-byte header precedes the payload (keeps 16-byte
 * alignment); large blocks are tracked in their own list so kfree can tell
 * them apart from arena blocks and validate the pointer. */
struct large_block {
    axys_size_t payload;
    axys_size_t frames;
    struct large_block *next;
    axys_uint64_t reserved;
};
AXYS_STATIC_ASSERT(sizeof(struct large_block) == 32u, large_header_must_be_32_bytes);

static struct large_block *large_head;
static struct block *heap_head;
static struct block *heap_tail;
static axys_size_t bytes_in_use;
static axys_size_t bytes_free;

static axys_size_t align_up(axys_size_t value, axys_size_t alignment)
{
    axys_size_t add = alignment - 1u;

    if (value > ~(axys_size_t)0 - add) {
        return 0;
    }
    return (value + add) & ~add;
}

/* The PMM owns contiguous-run search and accounting. Allocating frames one at
 * a time and hoping the search hint returns neighbours fails as soon as the
 * pool is fragmented, even when a large free run exists elsewhere. */
static axys_uint64_t alloc_contiguous_frames(axys_uint32_t count)
{
    if (count == 0 || count > HEAP_MAX_CONTIGUOUS_FRAMES) {
        return 0;
    }
    return axys_pmm_alloc_contiguous(count);
}

static int blocks_adjacent(const struct block *a, const struct block *b)
{
    const axys_uint8_t *end = (const axys_uint8_t *)(a + 1) + a->size;

    return end == (const axys_uint8_t *)b;
}

static void append_block(struct block *block)
{
    block->next = AXYS_NULL;
    if (heap_tail == AXYS_NULL) {
        heap_head = block;
    } else {
        heap_tail->next = block;
    }
    heap_tail = block;
    bytes_free += block->size;
}

/* Grow the heap by at least `min_bytes` of usable payload, preferring a full
 * HEAP_ARENA_FRAMES arena so repeated small allocations don't each cost a
 * fresh PMM call, but falling back to exactly what was asked for (rounded up
 * to a whole frame) if a full arena isn't available. */
static int heap_grow(axys_size_t min_bytes)
{
    axys_size_t needed;
    axys_uint32_t frames_needed;
    axys_uint32_t frames;
    axys_uint64_t base;

    if (min_bytes == 0 || min_bytes > HEAP_MAX_PAYLOAD) {
        return 0;
    }
    needed = min_bytes + sizeof(struct block);
    frames_needed = (axys_uint32_t)((needed + AXYS_PMM_FRAME_SIZE - 1u) /
                                    AXYS_PMM_FRAME_SIZE);
    frames = frames_needed > HEAP_ARENA_FRAMES ? frames_needed : HEAP_ARENA_FRAMES;
    if (frames > HEAP_MAX_CONTIGUOUS_FRAMES) {
        return 0;
    }
    base = alloc_contiguous_frames(frames);
    if (base == 0 && frames > frames_needed && frames_needed > 0) {
        base = alloc_contiguous_frames(frames_needed); /* smaller ask, still enough */
        frames = frames_needed;
    }
    if (base == 0 && frames_needed <= 1) {
        base = alloc_contiguous_frames(1);
        frames = 1;
    }
    if (base == 0) {
        return 0;
    }

    struct block *block = (struct block *)(axys_uintptr_t)base;

    axys_memset(block, 0, sizeof(*block));
    block->size = (axys_size_t)frames * AXYS_PMM_FRAME_SIZE - sizeof(struct block);
    block->free = 1;
    append_block(block);
    return 1;
}

void axys_heap_init(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&heap_lock);

    heap_head = AXYS_NULL;
    heap_tail = AXYS_NULL;
    large_head = AXYS_NULL;
    bytes_in_use = 0;
    bytes_free = 0;

    axys_spin_unlock_irqrestore(&heap_lock, flags);
}

/* Core allocator body; caller must hold heap_lock. */
static void *kmalloc_locked(axys_size_t size)
{
    if (size == 0) {
        return AXYS_NULL;
    }
    size = align_up(size, HEAP_ALIGN);
    if (size == 0) {
        return AXYS_NULL;
    }
    if (size > HEAP_MAX_PAYLOAD) {
        axys_size_t total = size + sizeof(struct large_block);
        axys_uint64_t frames;
        axys_uint64_t base;
        struct large_block *large;

        if (total < size) {
            return AXYS_NULL; /* overflow */
        }
        frames = ((axys_uint64_t)total + AXYS_PMM_FRAME_SIZE - 1u) / AXYS_PMM_FRAME_SIZE;
        base = axys_pmm_alloc_contiguous(frames);
        if (base == 0) {
            return AXYS_NULL;
        }
        large = (struct large_block *)(axys_uintptr_t)base;
        large->payload = size;
        large->frames = (axys_size_t)frames;
        large->reserved = 0;
        large->next = large_head;
        large_head = large;
        bytes_in_use += size;
        return (void *)(large + 1);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        for (struct block *block = heap_head; block != AXYS_NULL; block = block->next) {
            if (!block->free || block->size < size) {
                continue;
            }

            if (block->size >= size + sizeof(struct block) + HEAP_MIN_SPLIT_PAYLOAD) {
                axys_uint8_t *split_at = (axys_uint8_t *)(block + 1) + size;
                struct block *remainder = (struct block *)(void *)split_at;

                axys_memset(remainder, 0, sizeof(*remainder));
                remainder->size = block->size - size - sizeof(struct block);
                remainder->free = 1;
                remainder->next = block->next;
                block->next = remainder;
                block->size = size;
                bytes_free -= sizeof(struct block);
            }

            block->free = 0;
            bytes_free -= block->size;
            bytes_in_use += block->size;
            return (void *)(block + 1);
        }
        if (!heap_grow(size)) {
            return AXYS_NULL;
        }
    }
    return AXYS_NULL;
}

void *axys_kmalloc(axys_size_t size)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&heap_lock);
    void *pointer = kmalloc_locked(size);

    axys_spin_unlock_irqrestore(&heap_lock, flags);
    return pointer;
}

static struct block *find_block_by_payload(const void *pointer)
{
    for (struct block *block = heap_head; block != AXYS_NULL; block = block->next) {
        if ((const void *)(block + 1) == pointer) {
            return block;
        }
    }
    return AXYS_NULL;
}

void axys_kfree(void *pointer)
{
    if (pointer == AXYS_NULL) {
        return;
    }
    axys_uint64_t flags = axys_spin_lock_irqsave(&heap_lock);
    struct block *block;

    for (struct large_block **link = &large_head; *link != AXYS_NULL; link = &(*link)->next) {
        struct large_block *large = *link;

        if ((void *)(large + 1) == pointer) {
            *link = large->next;
            bytes_in_use -= large->payload;
            axys_spin_unlock_irqrestore(&heap_lock, flags);
            axys_pmm_free_contiguous((axys_uint64_t)(axys_uintptr_t)large, large->frames);
            return;
        }
    }
    block = find_block_by_payload(pointer);

    if (block == AXYS_NULL || block->free) {
        /* Unknown pointer or double free: refuse rather than corrupt the
         * list. A production kernel would also record the offender here. */
        axys_spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    block->free = 1;
    bytes_in_use -= block->size;
    bytes_free += block->size;

    /* Forward coalescing only: the list is singly linked, so merging with a
     * physically-preceding block would need a full-list scan to find it.
     * Good enough for a bump-style arena allocator; a doubly-linked list is
     * the natural upgrade if fragmentation becomes a real problem. */
    while (block->next != AXYS_NULL && block->next->free && blocks_adjacent(block, block->next)) {
        struct block *victim = block->next;

        /* The victim's header becomes usable payload of the merged block,
         * so total free bytes go UP by sizeof(struct block), not down: the
         * victim's own payload was already counted in bytes_free. */
        bytes_free += sizeof(struct block);
        block->size += sizeof(struct block) + victim->size;
        block->next = victim->next;
        if (victim == heap_tail) {
            heap_tail = block;
        }
    }
    axys_spin_unlock_irqrestore(&heap_lock, flags);
}

void *axys_kzalloc(axys_size_t size)
{
    void *pointer = axys_kmalloc(size);

    if (pointer != AXYS_NULL) {
        axys_memset(pointer, 0, size);
    }
    return pointer;
}

axys_size_t axys_heap_bytes_in_use(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&heap_lock);
    axys_size_t value = bytes_in_use;

    axys_spin_unlock_irqrestore(&heap_lock, flags);
    return value;
}

axys_size_t axys_heap_bytes_free(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&heap_lock);
    axys_size_t value = bytes_free;

    axys_spin_unlock_irqrestore(&heap_lock, flags);
    return value;
}

void axys_heap_selftest(void)
{
    void *a = axys_kmalloc(64);
    void *b = axys_kmalloc(128);
    void *c = axys_kmalloc(32);

    if (a == AXYS_NULL || b == AXYS_NULL || c == AXYS_NULL) {
        axys_panic("heap: selftest allocation failed");
    }
    if (a == b || b == c || a == c) {
        axys_panic("heap: selftest got overlapping pointers");
    }

    axys_memset(a, 0xaa, 64);
    axys_memset(b, 0xbb, 128);
    axys_memset(c, 0xcc, 32);
    if (((axys_uint8_t *)a)[0] != 0xaa || ((axys_uint8_t *)b)[0] != 0xbb ||
        ((axys_uint8_t *)c)[0] != 0xcc) {
        axys_panic("heap: selftest writes did not stick");
    }

    axys_size_t used_before_free = axys_heap_bytes_in_use();

    axys_kfree(b);
    if (axys_heap_bytes_in_use() != used_before_free - 128u) {
        axys_panic("heap: selftest bytes_in_use wrong after free");
    }
    axys_kfree(b); /* double free must be a no-op */
    if (axys_heap_bytes_in_use() != used_before_free - 128u) {
        axys_panic("heap: selftest double-free changed accounting");
    }

    void *d = axys_kmalloc(96); /* should reuse b's freed block */
    if (d != b) {
        axys_panic("heap: selftest did not reuse a freed block");
    }

    axys_kfree(a);
    axys_kfree(c);
    axys_kfree(d);

    /* One large allocation to force heap_grow beyond a single default arena. */
    void *big = axys_kmalloc(AXYS_PMM_FRAME_SIZE * 3u);

    if (big == AXYS_NULL) {
        axys_panic("heap: selftest large growth allocation failed");
    }
    axys_memset(big, 0x42, AXYS_PMM_FRAME_SIZE * 3u);
    axys_kfree(big);

    axys_printf("heap: selftest passed (%u bytes in use, %u bytes free)\n",
                (axys_uint32_t)axys_heap_bytes_in_use(),
                (axys_uint32_t)axys_heap_bytes_free());
}
