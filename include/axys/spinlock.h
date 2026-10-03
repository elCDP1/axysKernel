#ifndef AXYS_SPINLOCK_H
#define AXYS_SPINLOCK_H

#include "axys/cpu.h"
#include "axys/types.h"

/*
 * Interrupt-safe spinlock.
 *
 * This kernel is currently UP, so the only concurrency an allocator or a
 * statistics counter has to survive is "a driver's critical section vs. an
 * interrupt handler re-entering the same code". Disabling local interrupts for
 * the duration of the section closes that window; when SMP bring-up lands, a
 * per-CPU owner field and an atomic exchange are added underneath this same
 * interface and no caller changes.
 *
 * The lock word uses bit 0 as the held flag and bit 1 as "waiter pending".
 * Releasing with a waiter present does one uncontended xchg instead of making
 * every waiting core poll the cache line hot: contenders set the waiter bit
 * while spinning, so the holder knows it must not assume exclusivity on the
 * plain store and must hand the line over explicitly.
 *
 * Locks must start in the unlocked (zero) state. Statically initialised locks
 * satisfy this by construction: boot.S zeroes .bss before any C code runs, and
 * explicit `= AXYS_SPINLOCK_UNLOCKED` initialisers land in .data. Either way
 * the guarantee is "zero word at first use", not a particular linker section.
 */

#define AXYS_SPINLOCK_UNLOCKED 0u

struct axys_spinlock {
    volatile axys_uint32_t word;
};

#define AXYS_SPINLOCK_INIT(name)                                               \
    {                                                                          \
        .word = AXYS_SPINLOCK_UNLOCKED                                         \
    }

static AXYS_ALWAYS_INLINE void axys_spin_lock_raw(struct axys_spinlock *lock)
{
    axys_uint32_t previous;

    /* Fast path: uncontended acquisition is a single locked xchg. */
    __asm__ volatile("lock xchgl %0, %1"
                     : "=r"(previous), "+m"(lock->word)
                     : "0"(1u)
                     : "memory");
    if ((previous & 1u) == 0u) {
        return;
    }

    /* Contended: spin reading the word (no locked traffic, so the line stays
     * Shared in every waiter's cache), then announce ourselves as a waiter
     * with a locked or -- the bit tells the releasing side that a plain store
     * handover is not safe -- and retry the exchange. Losing the race costs
     * nothing except another pass: the waiter bit stays set while we are
     * still queuing, which is exactly what it means. */
    do {
        while ((lock->word & 1u) != 0u) {
            axys_cpu_relax();
        }
        __asm__ volatile("lock orl $2, %0"
                         : "+m"(lock->word)
                         :
                         : "memory");
        __asm__ volatile("lock xchgl %0, %1"
                         : "=r"(previous), "+m"(lock->word)
                         : "0"(1u)
                         : "memory");
    } while ((previous & 1u) != 0u);
}

static AXYS_ALWAYS_INLINE void axys_spin_unlock_raw(struct axys_spinlock *lock)
{
    axys_uint32_t release_value;
    axys_uint32_t discard;

    /* With no waiter queued, a compiler barrier plus a plain store is enough.
     * With a waiter, the line is Shared in the waiters' caches, so clearing
     * the held bit with an ordinary store could let two cores observe the
     * lock as free at once; hand it over with one uncontended exchange
     * instead, keeping the waiter bit so the next acquirer still sees it. */
    __asm__ volatile("" : "+m"(lock->word));
    release_value = (lock->word & 2u) != 0u ? 2u : 0u;
    if (release_value == 0u) {
        __asm__ volatile("" : : : "memory");
        lock->word = 0u;
        return;
    }
    __asm__ volatile("xchgl %0, %1"
                     : "=r"(discard), "+m"(lock->word)
                     : "0"(release_value)
                     : "memory");
}

/* Bracketed forms: save/restore RFLAGS around the section so nesting and
 * calling context (interrupts already off, e.g. from a fault handler) are
 * both handled correctly. */
static AXYS_ALWAYS_INLINE axys_uint64_t
axys_spin_lock_irqsave(struct axys_spinlock *lock)
{
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    axys_spin_lock_raw(lock);
    return flags;
}

static AXYS_ALWAYS_INLINE void
axys_spin_unlock_irqrestore(struct axys_spinlock *lock, axys_uint64_t flags)
{
    axys_spin_unlock_raw(lock);
    axys_cpu_restore_flags(flags);
}

#endif
