/*
 * Heap allocator invariants. heap.c is included directly so the test can walk
 * the real block list: after every operation the list must account for every
 * byte of every arena that the (stubbed) physical allocator handed out, the
 * tail pointer must be the last block, and bytes_in_use must equal the sum of
 * the allocated block sizes. Hosted build only.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AXYS_SPINLOCK_H /* replace the privileged cli/sti spinlock */
#include "axys/types.h"
struct axys_spinlock {
    volatile axys_uint32_t word;
};
#define AXYS_SPINLOCK_INIT(name) {.word = 0}
static inline axys_uint64_t axys_spin_lock_irqsave(struct axys_spinlock *l) { (void)l; return 0; }
static inline void axys_spin_unlock_irqrestore(struct axys_spinlock *l, axys_uint64_t f) { (void)l; (void)f; }

#include "axys/printf.h"
#include "axys/panic.h"
#include "axys/pmm.h"

static size_t arena_bytes_handed_out;

axys_uint64_t axys_pmm_alloc_contiguous(axys_uint64_t count)
{
    void *p = aligned_alloc(4096, (size_t)count * 4096);

    if (p == NULL) {
        return 0;
    }
    memset(p, 0xa5, (size_t)count * 4096); /* garbage, like real RAM */
    arena_bytes_handed_out += (size_t)count * 4096;
    return (axys_uint64_t)(uintptr_t)p;
}

void axys_pmm_free_contiguous(axys_uint64_t address, axys_uint64_t count)
{
    arena_bytes_handed_out -= (size_t)count * 4096;
    free((void *)(uintptr_t)address);
}

int axys_printf(const char *f, ...) { (void)f; return 0; }
void axys_panic(const char *m) { fprintf(stderr, "panic: %s\n", m); abort(); }

#include "../kernel/heap.c"

static void check_invariants(const char *where)
{
    size_t listed = 0, alloc_sum = 0, free_sum = 0, large_sum = 0, large_bytes = 0;
    struct block *last = NULL;

    for (struct block *b = heap_head; b != NULL; b = b->next) {
        listed += b->size + sizeof(struct block);
        if (b->free) free_sum += b->size; else alloc_sum += b->size;
        last = b;
    }
    for (struct large_block *l = large_head; l != NULL; l = l->next) {
        large_sum += l->payload;
        large_bytes += l->frames * 4096;
    }
    if (last != heap_tail) {
        fprintf(stderr, "FAIL (%s): heap_tail is not the last block in the list\n", where);
        exit(1);
    }
    if (listed + large_bytes != arena_bytes_handed_out) {
        fprintf(stderr, "FAIL (%s): %zu bytes of arena are unreachable from the block list (listed %zu, handed out %zu)\n",
                where, arena_bytes_handed_out - listed - large_bytes, listed, arena_bytes_handed_out);
        exit(1);
    }
    if (bytes_in_use != alloc_sum + large_sum) {
        fprintf(stderr, "FAIL (%s): bytes_in_use %zu but allocated blocks sum to %zu\n", where, bytes_in_use,
                alloc_sum + large_sum);
        exit(1);
    }
    if (bytes_free != free_sum) {
        fprintf(stderr, "FAIL (%s): bytes_free %zu but free blocks sum to %zu\n", where, bytes_free, free_sum);
        exit(1);
    }
}

static uint64_t rng = 88172645463325252ull;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng % n);
}

int main(int argc, char **argv)
{
    enum { SLOTS = 400 };
    static struct { unsigned char *p; size_t n; unsigned char tag; } slot[SLOTS];
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 200000;

    axys_heap_init();
    for (unsigned r = 0; r < rounds; ++r) {
        unsigned i = rnd(SLOTS);

        if (slot[i].p != NULL) {
            for (size_t k = 0; k < slot[i].n; ++k) {
                if (slot[i].p[k] != slot[i].tag) {
                    fprintf(stderr, "FAIL: allocation %u was overwritten at byte %zu\n", i, k);
                    return 1;
                }
            }
            axys_kfree(slot[i].p);
            slot[i].p = NULL;
        } else {
            size_t n;
            switch (rnd(10)) {
            case 0: n = 1 + rnd(16); break;
            case 1: case 2: case 3: n = 16 + rnd(120); break;
            case 4: case 5: n = 100 + rnd(1500); break;
            case 6: case 7: n = 1000 + rnd(3000); break;
            case 8: n = 3000 + rnd(9000); break;
            default: n = 8000 + rnd(40000); break; /* some take the large-block path */
            }
            slot[i].p = rnd(2) ? axys_kmalloc(n) : axys_kzalloc(n);
            slot[i].n = n;
            slot[i].tag = (unsigned char)(1 + rnd(250));
            if (slot[i].p != NULL) {
                if (((uintptr_t)slot[i].p & 15u) != 0) { fprintf(stderr, "FAIL: misaligned allocation\n"); return 1; }
                memset(slot[i].p, slot[i].tag, n);
            }
        }
        if (r % 7 == 0) check_invariants("random ops");
    }
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (slot[i].p) axys_kfree(slot[i].p);
    }
    check_invariants("after freeing everything");
    if (bytes_in_use != 0) {
        fprintf(stderr, "FAIL: bytes_in_use is %zu with nothing allocated\n", bytes_in_use);
        return 1;
    }
    puts("test_heap: ok");
    return 0;
}
