#include "axys/pmm.h"
#include "axys/panic.h"
#include "axys/printf.h"
#include "axys/spinlock.h"
#include "axys/string.h"
#include "axys/vmm.h"

/* Linker-provided symbols marking the kernel's own physical footprint (see
 * scripts/linker.ld). These are addresses, not variables -- take &symbol, or
 * equivalently use the array-decay below, never *symbol. */
extern axys_uint8_t __kernel_start[];
extern axys_uint8_t __kernel_end[];

#define INITIAL_BITMAP_BITS AXYS_PMM_INITIAL_FRAMES
#define INITIAL_BITMAP_BYTES (INITIAL_BITMAP_BITS / 8u)
#define INITIAL_BITMAP_WORDS (INITIAL_BITMAP_BYTES / sizeof(axys_uint64_t))
#define BITS_PER_WORD 64u

AXYS_STATIC_ASSERT(INITIAL_BITMAP_BITS % BITS_PER_WORD == 0, bitmap_whole_words);
AXYS_STATIC_ASSERT(sizeof(axys_uint64_t) * 8u == BITS_PER_WORD, word_is_64_bits);

/* 1 = frame reserved/used, 0 = frame free. Starts fully reserved; init()
 * clears bits only for frames the firmware map actually offers, within the
 * range that is actually identity-mapped.
 *
 * Word-based rather than byte-based: allocation scans for a zero bit and a
 * full 64-bit word either skips in one compare or yields the first free frame
 * via __builtin_ctzll in a single instruction. The previous byte-wise loop
 * tested eight bits per load with no vector width at all.
 *
 * `allocated_bitmap` mirrors every currently-allocated frame so a free can
 * distinguish "in use" from "already free" without racing the main bitmap:
 * the main bitmap alone says only reserved-vs-free, and a fresh alloc between
 * a stale double-free's test and its clear would be undone by that clear.
 * Both bitmaps, the counters and the hint are updated under pmm_lock. */
static axys_uint64_t initial_bitmap[INITIAL_BITMAP_WORDS];
static axys_uint64_t initial_allocated_bitmap[INITIAL_BITMAP_WORDS];
static axys_uint64_t *bitmap = initial_bitmap;
static axys_uint64_t *allocated_bitmap = initial_allocated_bitmap;
static axys_size_t bitmap_bytes = INITIAL_BITMAP_BYTES;
static axys_uint64_t identity_limit;   /* bytes; frames at/above this are off-limits */
static axys_uint64_t total_frames;     /* identity_limit / AXYS_PMM_FRAME_SIZE */
static axys_uint64_t free_frames;
static axys_uint64_t search_hint; /* first frame that might still be free */
static int pmm_ready;

/* Interrupt-safe: kmalloc paths run while drivers have IRQs live, and the
 * tick handler may itself allocate. One lock covers both bitmaps and the
 * counters so they can never disagree mid-update. */
static struct axys_spinlock pmm_lock = AXYS_SPINLOCK_INIT(pmm_lock);

static inline int bit_test(axys_uint64_t frame)
{
    return (bitmap[frame >> 6u] >> (frame & 63u)) & 1u;
}

static inline void bit_set(axys_uint64_t frame)
{
    bitmap[frame >> 6u] |= (axys_uint64_t)1u << (frame & 63u);
}

static inline void bit_clear(axys_uint64_t frame)
{
    bitmap[frame >> 6u] &= ~((axys_uint64_t)1u << (frame & 63u));
}

static inline int allocated_bit_test(axys_uint64_t frame)
{
    return (allocated_bitmap[frame >> 6u] >> (frame & 63u)) & 1u;
}

static inline void allocated_bit_set(axys_uint64_t frame)
{
    allocated_bitmap[frame >> 6u] |= (axys_uint64_t)1u << (frame & 63u);
}

static inline void allocated_bit_clear(axys_uint64_t frame)
{
    allocated_bitmap[frame >> 6u] &= ~((axys_uint64_t)1u << (frame & 63u));
}

/* Set (`ones` non-zero) or clear every bit for frames [first, last), whole
 * words written directly and partial edge words with masked read-modify-
 * writes. Boot-time reservations span thousands of frames; the old per-frame
 * loop cost one byte RMW each, this costs one store per 64 frames. */
static void bitmap_fill(axys_uint64_t *map, axys_uint64_t first, axys_uint64_t last,
                       int ones)
{
    axys_uint64_t pattern = ones ? ~(axys_uint64_t)0 : (axys_uint64_t)0;

    if (first >= last) {
        return;
    }

    /* Leading partial word. */
    if ((first & 63u) != 0u) {
        axys_uint64_t lead_bits = 64u - (first & 63u);
        axys_uint64_t span = last - first;
        axys_uint64_t width = span < lead_bits ? span : lead_bits;
        axys_uint64_t mask = width >= 64u
                                 ? ~(axys_uint64_t)0
                                 : (((axys_uint64_t)1 << width) - 1u) << (first & 63u);

        map[first >> 6u] = (map[first >> 6u] & ~mask) | (pattern & mask);
        first += width;
        if (first >= last) {
            return;
        }
    }

    /* Whole words. */
    while (last - first >= BITS_PER_WORD) {
        map[first >> 6u] = pattern;
        first += BITS_PER_WORD;
    }

    /* Trailing partial word. */
    if (first < last) {
        axys_uint64_t tail = last - first;
        axys_uint64_t mask = tail >= 64u ? ~(axys_uint64_t)0
                                         : (((axys_uint64_t)1 << tail) - 1u);

        map[first >> 6u] = (map[first >> 6u] & ~mask) | (pattern & mask);
    }
}

/* Portable 64-bit population count. GCC may emit a libgcc call for
 * __builtin_popcountll when the target lacks POPCNT, and a freestanding
 * kernel links without libgcc -- so the shift-and-add fallback is spelled
 * out here instead. (The ctzll user in pmm_alloc_frame is safe: GCC lowers
 * it to inline BMI1/BSF+TZCNT sequences with no library dependency.) */
static axys_uint64_t popcount64(axys_uint64_t x)
{
    x -= (x >> 1u) & 0x5555555555555555ull;
    x = (x & 0x3333333333333333ull) + ((x >> 2u) & 0x3333333333333333ull);
    x = (x + (x >> 4u)) & 0x0f0f0f0f0f0f0f0full;
    return (x * 0x0101010101010101ull) >> 56u;
}

/* Count zero bits over [0, total_frames), word-wise, masking off positions
 * past the mapped limit so unmanaged frames never count as free. */
static axys_uint64_t count_free_frames(void)
{
    axys_uint64_t count = 0;
    axys_uint64_t words = (total_frames + BITS_PER_WORD - 1u) / BITS_PER_WORD;

    for (axys_uint64_t word = 0; word < words; ++word) {
        axys_uint64_t base = word * BITS_PER_WORD;
        axys_uint64_t valid = base + BITS_PER_WORD <= total_frames
                                  ? BITS_PER_WORD
                                  : total_frames - base;
        axys_uint64_t mask = valid >= BITS_PER_WORD
                                 ? ~(axys_uint64_t)0
                                 : (((axys_uint64_t)1 << valid) - 1u);

        count += popcount64(~bitmap[word] & mask);
    }
    return count;
}

/* Mark every whole frame inside [base, base+length) reserved (used). Partial
 * frames at either edge are rounded outward so a reservation never leaves a
 * sliver of a protected region marked free. Clipped to identity_limit. */
static void reserve_range(axys_uint64_t base, axys_uint64_t length)
{
    axys_uint64_t end;
    axys_uint64_t first;
    axys_uint64_t last;

    if (length == 0 || base >= identity_limit) {
        return;
    }
    end = length > identity_limit - base ? identity_limit : base + length;
    if (base >= end) {
        return;
    }

    first = base / AXYS_PMM_FRAME_SIZE;
    last = (end + AXYS_PMM_FRAME_SIZE - 1u) / AXYS_PMM_FRAME_SIZE;
    if (last > total_frames) {
        last = total_frames;
    }
    bitmap_fill(bitmap, first, last, 1);
}

static void free_range(axys_uint64_t base, axys_uint64_t length)
{
    axys_uint64_t end;
    axys_uint64_t first;
    axys_uint64_t last;

    if (length == 0 || base >= identity_limit) {
        return;
    }
    end = length > identity_limit - base ? identity_limit : base + length;
    if (base >= end) {
        return;
    }

    /* Round the beginning inward without overflowing base + frame_size - 1. */
    first = base / AXYS_PMM_FRAME_SIZE;
    if ((base % AXYS_PMM_FRAME_SIZE) != 0) {
        ++first;
    }
    last = end / AXYS_PMM_FRAME_SIZE;
    if (last > total_frames) {
        last = total_frames;
    }
    bitmap_fill(bitmap, first, last, 0);
}

void axys_pmm_init(const struct axys_boot_info *info)
{
    axys_size_t index;
    identity_limit = axys_vmm_extend_identity(AXYS_PMM_INITIAL_LIMIT_BYTES);
    if (identity_limit > AXYS_PMM_INITIAL_LIMIT_BYTES) {
        identity_limit = AXYS_PMM_INITIAL_LIMIT_BYTES;
    }
    total_frames = identity_limit / AXYS_PMM_FRAME_SIZE;

    bitmap = initial_bitmap;
    allocated_bitmap = initial_allocated_bitmap;
    bitmap_bytes = INITIAL_BITMAP_BYTES;
    axys_memset(bitmap, 0xff, bitmap_bytes);
    axys_memset(allocated_bitmap, 0, bitmap_bytes);
    free_frames = 0;
    search_hint = 0;
    pmm_ready = 0;

    if (info != AXYS_NULL) {
        for (index = 0; index < info->mmap_entries; ++index) {
            const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);

            if (entry == AXYS_NULL || entry->type != AXYS_MMAP_AVAILABLE) {
                continue;
            }
            free_range(entry->address, entry->length);
        }
        /* A malformed/overlapping map must never turn a firmware-reserved
         * region into allocatable RAM. Reserved types win over available. */
        for (index = 0; index < info->mmap_entries; ++index) {
            const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);

            if (entry == AXYS_NULL || entry->type == AXYS_MMAP_AVAILABLE) {
                continue;
            }
            reserve_range(entry->address, entry->length);
        }
    }

    /* Unconditional reservations, regardless of what the map says: */
    reserve_range(0, 0x100000ULL); /* low 1 MiB: real-mode IVT, BDA, VGA, BIOS */
    reserve_range((axys_uint64_t)(axys_uintptr_t)__kernel_start,
                  (axys_uint64_t)((axys_uintptr_t)__kernel_end -
                                  (axys_uintptr_t)__kernel_start));
    if (info != AXYS_NULL && info->info_address != 0) {
        axys_uint64_t size = info->info_total_bytes != 0 ? info->info_total_bytes
                                                          : 65536ULL;
        reserve_range((axys_uint64_t)info->info_address, size);
    }

    /* Multiboot modules are bootloader-owned memory and may overlap ranges
     * that the firmware map otherwise calls available. Never hand an initrd or
     * another boot module back to the allocator before user space has consumed
     * it. */
    if (info != AXYS_NULL) {
        for (index = 0; index < info->module_count; ++index) {
            const struct axys_mb2_module *module = &info->modules[index];
            reserve_range(module->start, (axys_uint64_t)module->end - module->start);
        }
        if (info->framebuffer_address != 0 && info->framebuffer_pitch != 0 &&
            info->framebuffer_height != 0) {
            axys_uint64_t framebuffer_bytes =
                (axys_uint64_t)info->framebuffer_pitch * info->framebuffer_height;
            reserve_range(info->framebuffer_address, framebuffer_bytes);
        }
    }

    free_frames = count_free_frames();
    pmm_ready = 1;
}

/* Grow the frame metadata only after the low 4 GiB allocator is live. This
 * lets VMM allocate its own page-table pages below 4 GiB and ensures the new
 * bitmap storage is first marked busy in the bootstrap bitmap, then copied. */
int axys_pmm_extend(const struct axys_boot_info *info)
{
    const axys_uint64_t old_limit = identity_limit;
    axys_uint64_t high_limit;
    axys_uint64_t new_limit;
    axys_uint64_t new_frames;
    axys_uint64_t words;
    axys_uint64_t new_bytes;
    axys_uint64_t bitmap_frames;
    axys_uint64_t bitmap_phys;
    axys_uint64_t allocated_phys;
    axys_uint64_t *new_bitmap;
    axys_uint64_t *new_allocated;
    axys_size_t index;

    if (!pmm_ready || info == AXYS_NULL) {
        return -1;
    }
    high_limit = axys_mmap_highest_address(info);
    if (high_limit <= old_limit) {
        return 0;
    }
    if (high_limit > axys_vmm_physical_limit()) {
        high_limit = axys_vmm_physical_limit();
    }
    if (high_limit > AXYS_VMM_MAX_PHYSICAL_BYTES) {
        high_limit = AXYS_VMM_MAX_PHYSICAL_BYTES;
    }
    if (high_limit > ~(axys_uint64_t)0 - (AXYS_PMM_FRAME_SIZE - 1u)) {
        return -1;
    }
    new_limit = (high_limit + AXYS_PMM_FRAME_SIZE - 1u) &
                ~((axys_uint64_t)AXYS_PMM_FRAME_SIZE - 1u);
    if (new_limit <= old_limit) {
        return 0;
    }

    if (axys_vmm_map_high_ram(info, new_limit) != 0) {
        return -1;
    }

    new_frames = new_limit / AXYS_PMM_FRAME_SIZE;
    words = (new_frames + BITS_PER_WORD - 1u) / BITS_PER_WORD;
    if (words > ~(axys_uint64_t)0 / sizeof(axys_uint64_t)) {
        return -1;
    }
    new_bytes = words * sizeof(axys_uint64_t);
    bitmap_frames = (new_bytes + AXYS_PMM_FRAME_SIZE - 1u) /
                    AXYS_PMM_FRAME_SIZE;
    if (bitmap_frames == 0 || bitmap_frames > free_frames / 2u) {
        return -1;
    }

    bitmap_phys = axys_pmm_alloc_contiguous(bitmap_frames);
    if (bitmap_phys == 0) {
        return -1;
    }
    allocated_phys = axys_pmm_alloc_contiguous(bitmap_frames);
    if (allocated_phys == 0) {
        axys_pmm_free_contiguous(bitmap_phys, bitmap_frames);
        return -1;
    }

    new_bitmap = (axys_uint64_t *)(axys_uintptr_t)bitmap_phys;
    new_allocated = (axys_uint64_t *)(axys_uintptr_t)allocated_phys;
    axys_memset(new_bitmap, 0xff, (axys_size_t)bitmap_frames * AXYS_PMM_FRAME_SIZE);
    axys_memset(new_allocated, 0, (axys_size_t)bitmap_frames * AXYS_PMM_FRAME_SIZE);
    axys_memcpy(new_bitmap, bitmap, bitmap_bytes);
    axys_memcpy(new_allocated, allocated_bitmap, bitmap_bytes);

    /* Switch to the larger bitmaps before adding high ranges. Existing low
     * allocations, including these bitmap pages and VMM tables, are retained
     * by the copied allocated bitmap. */
    bitmap = new_bitmap;
    allocated_bitmap = new_allocated;
    bitmap_bytes = (axys_size_t)new_bytes;
    total_frames = new_frames;
    identity_limit = new_limit;

    if (info->mmap != AXYS_NULL) {
        for (index = 0; index < info->mmap_entries; ++index) {
            const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);
            axys_uint64_t base;
            axys_uint64_t end;

            if (entry == AXYS_NULL || entry->length == 0 ||
                entry->address + entry->length < entry->address) {
                continue;
            }
            base = entry->address;
            end = entry->address + entry->length;
            if (base < old_limit) {
                base = old_limit;
            }
            if (end > new_limit) {
                end = new_limit;
            }
            if (base >= end) {
                continue;
            }
            if (entry->type == AXYS_MMAP_AVAILABLE) {
                free_range(base, end - base);
            }
        }
        /* Reserved firmware ranges win if a malformed map overlaps usable
         * RAM. Never let that overlap expose a device aperture as allocatable. */
        for (index = 0; index < info->mmap_entries; ++index) {
            const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);
            axys_uint64_t base;
            axys_uint64_t end;

            if (entry == AXYS_NULL || entry->type == AXYS_MMAP_AVAILABLE ||
                entry->length == 0 || entry->address + entry->length < entry->address) {
                continue;
            }
            base = entry->address < old_limit ? old_limit : entry->address;
            end = entry->address + entry->length;
            if (end > new_limit) {
                end = new_limit;
            }
            if (base < end) {
                reserve_range(base, end - base);
            }
        }
    }
    if (info->framebuffer_address != 0 && info->framebuffer_pitch != 0 &&
        info->framebuffer_height != 0) {
        axys_uint64_t framebuffer_bytes =
            (axys_uint64_t)info->framebuffer_pitch * info->framebuffer_height;
        reserve_range(info->framebuffer_address, framebuffer_bytes);
    }
    /* The copied allocated bitmap only records what was reserved before the
     * extension, and the freshly freed "available" ranges above old_limit may
     * cover bootloader-owned memory. Re-assert the reservations axys_pmm_init
     * made for the multiboot info block and the boot modules, or the initrd
     * could be handed out as free frames. */
    if (info->info_address != 0) {
        axys_uint64_t info_bytes = info->info_total_bytes != 0 ? info->info_total_bytes
                                                              : 65536ULL;

        reserve_range((axys_uint64_t)info->info_address, info_bytes);
    }
    for (index = 0; index < info->module_count; ++index) {
        const struct axys_mb2_module *module = &info->modules[index];

        reserve_range(module->start, (axys_uint64_t)module->end - module->start);
    }

    free_frames = count_free_frames();
    search_hint = old_limit / AXYS_PMM_FRAME_SIZE;
    if (search_hint >= total_frames) {
        search_hint = 0;
    }
    return 0;
}

/* Caller holds pmm_lock. Returns the frame index or ~0 when out of memory. */
static axys_uint64_t scan_free_frame(void)
{
    axys_uint64_t last_word;
    axys_uint64_t word;
    unsigned start_bit;
    axys_uint64_t pass;

    /* An empty pool is a legitimate (if useless) configuration: report OOM
     * instead of letting (total_frames - 1) underflow below. */
    if (total_frames == 0) {
        return ~(axys_uint64_t)0;
    }

    last_word = (total_frames - 1u) >> 6u;
    word = search_hint >> 6u;
    start_bit = (unsigned)(search_hint & 63u);

    /* Two passes: from the hint to the top, then wrapped from 0 to the hint.
     * The wrap keeps recently-freed low frames reachable after the hint has
     * drifted to the end of a nearly-full pool. */
    for (pass = 0; pass < 2; ++pass) {
        for (; word <= last_word; ++word, start_bit = 0u) {
            axys_uint64_t used = bitmap[word];
            axys_uint64_t free_mask;
            axys_uint64_t candidate;

            if (start_bit != 0u) {
                used |= ((((axys_uint64_t)1 << start_bit)) - 1u);
            }
            free_mask = ~used;
            if (word == last_word && (total_frames & 63u) != 0u) {
                /* Positions past total_frames read as "free" only because the
                 * bitmap was memset to all-ones and never touched; constrain
                 * the *free* side so ctzll can never return one. Masking the
                 * used side instead would have inverted the logic. */
                free_mask &= (((axys_uint64_t)1 << (total_frames & 63u)) - 1u);
            }
            if (free_mask == 0) {
                continue; /* whole word busy (or beyond the frame limit) */
            }
            candidate = (axys_uint64_t)__builtin_ctzll(free_mask);
            return word * BITS_PER_WORD + candidate;
        }
        word = 0;
        start_bit = 0u;
        if (search_hint >> 6u > last_word) {
            break; /* hint already past the end on the first pass: sweep once */
        }
    }
    return ~(axys_uint64_t)0;
}

axys_uint64_t axys_pmm_alloc_frame(void)
{
    axys_uint64_t flags;
    axys_uint64_t frame;

    if (!pmm_ready) {
        return 0;
    }

    flags = axys_spin_lock_irqsave(&pmm_lock);
    frame = scan_free_frame();
    if (frame == ~(axys_uint64_t)0) {
        axys_spin_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }
    bit_set(frame);
    allocated_bit_set(frame);
    --free_frames;
    search_hint = frame + 1u;
    if (search_hint >= total_frames) {
        search_hint = 0;
    }
    axys_spin_unlock_irqrestore(&pmm_lock, flags);
    return frame * AXYS_PMM_FRAME_SIZE;
}

void axys_pmm_free_frame(axys_uint64_t address)
{
    axys_uint64_t flags;
    axys_uint64_t frame;

    if (!pmm_ready || (address & (AXYS_PMM_FRAME_SIZE - 1u)) != 0) {
        return;
    }
    frame = address / AXYS_PMM_FRAME_SIZE;
    if (frame >= total_frames) {
        return;
    }

    flags = axys_spin_lock_irqsave(&pmm_lock);
    if (allocated_bit_test(frame)) {
        /* Only frames handed out by this allocator may return to the pool;
         * reserved and never-allocated frames stay put, which also makes a
         * double free a no-op instead of a use-after-free enabler. */
        allocated_bit_clear(frame);
        bit_clear(frame);
        ++free_frames;
        if (frame < search_hint) {
            search_hint = frame;
        }
    }
    axys_spin_unlock_irqrestore(&pmm_lock, flags);
}

axys_uint64_t axys_pmm_alloc_contiguous(axys_uint64_t count)
{
    axys_uint64_t flags;
    axys_uint64_t run = 0;
    axys_uint64_t start = 0;

    if (!pmm_ready || count == 0 || count > free_frames) {
        return 0;
    }
    flags = axys_spin_lock_irqsave(&pmm_lock);
    for (axys_uint64_t frame = 0; frame < total_frames; ++frame) {
        if (bit_test(frame)) {
            run = 0;
            continue;
        }
        if (run == 0) {
            start = frame;
        }
        if (++run == count) {
            bitmap_fill(bitmap, start, start + count, 1);
            bitmap_fill(allocated_bitmap, start, start + count, 1);
            free_frames -= count;
            axys_spin_unlock_irqrestore(&pmm_lock, flags);
            return start * AXYS_PMM_FRAME_SIZE;
        }
    }
    axys_spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

void axys_pmm_free_contiguous(axys_uint64_t address, axys_uint64_t count)
{
    for (axys_uint64_t i = 0; i < count; ++i) {
        axys_pmm_free_frame(address + i * AXYS_PMM_FRAME_SIZE);
    }
}

axys_uint64_t axys_pmm_total_frames(void)
{
    return total_frames;
}

axys_uint64_t axys_pmm_free_frames(void)
{
    return free_frames;
}

axys_uint64_t axys_pmm_used_frames(void)
{
    return total_frames - free_frames;
}

axys_uint64_t axys_pmm_identity_limit(void)
{
    return identity_limit;
}

void axys_pmm_selftest(void)
{
    axys_uint64_t before_free = free_frames;

    /* Frame 0 is unconditionally reserved. Releasing a reserved frame must
     * not make it allocatable. */
    axys_pmm_free_frame(0);
    if (free_frames != before_free) {
        axys_panic("pmm: freeing reserved frame changed free count");
    }

    axys_uint64_t a = axys_pmm_alloc_frame();
    axys_uint64_t b = axys_pmm_alloc_frame();

    if (a == 0 || b == 0) {
        axys_panic("pmm: selftest could not allocate frames");
    }
    if (a == b) {
        axys_panic("pmm: selftest got the same frame twice");
    }
    if (identity_limit > AXYS_PMM_INITIAL_LIMIT_BYTES &&
        (a < AXYS_PMM_INITIAL_LIMIT_BYTES || b < AXYS_PMM_INITIAL_LIMIT_BYTES)) {
        axys_panic("pmm: high-memory allocator did not return a frame above 4 GiB");
    }
    if (a % AXYS_PMM_FRAME_SIZE != 0 || b % AXYS_PMM_FRAME_SIZE != 0) {
        axys_panic("pmm: selftest frame not page-aligned");
    }
    if (free_frames != before_free - 2) {
        axys_panic("pmm: selftest free count wrong after alloc");
    }

    axys_pmm_free_frame(a);
    if (free_frames != before_free - 1) {
        axys_panic("pmm: selftest free count wrong after free");
    }
    axys_pmm_free_frame(a); /* double free must be a no-op */
    if (free_frames != before_free - 1) {
        axys_panic("pmm: selftest double-free changed the count");
    }

    axys_uint64_t c = axys_pmm_alloc_frame();
    if (c != a) {
        axys_panic("pmm: selftest freed frame was not reused");
    }
    axys_pmm_free_frame(b);
    axys_pmm_free_frame(c);
    if (free_frames != before_free) {
        axys_panic("pmm: selftest free count did not return to baseline");
    }

    axys_printf("pmm: selftest passed (%u/%u frames free, physical limit %u MiB)\n",
                (axys_uint32_t)free_frames, (axys_uint32_t)total_frames,
                (axys_uint32_t)(identity_limit / (1024u * 1024u)));
}
