#include "axys/panic.h"
#include "axys/random.h"
#include "axys/types.h"

/*
 * Kernel stack protector. Every function with a local array (or address-taken
 * local) gets a canary between its buffers and its return address; the check
 * on return calls __stack_chk_fail() if a buffer overflow overwrote it. The
 * kernel is built with -mstack-protector-guard=global, so the canary is this
 * one variable rather than a per-thread TLS slot (there is no TLS in the
 * kernel). It starts with a fixed value so early boot code is protected too,
 * and is replaced by a random one as soon as the RNG is up.
 */
axys_uintptr_t __stack_chk_guard = (axys_uintptr_t)0x595e9fbd94fda700ULL;

#define NO_SSP __attribute__((no_stack_protector))

NO_SSP AXYS_NORETURN void __stack_chk_fail(void)
{
    axys_panic("stack smashing detected");
}

/* Must be called only when no protected frame is live on the stack (the guard
 * is compared against the value stored at function entry). kmain calls it
 * right after the RNG comes up, and kmain itself is NO_SSP. The low byte is
 * forced to zero so a string overflow cannot copy the canary intact. */
NO_SSP int axys_ssp_reseed(void)
{
    axys_uint64_t value = 0;

    if (axys_random_u64(&value) != 0) {
        return -1;
    }
    __stack_chk_guard = (axys_uintptr_t)(value & ~0xffULL);
    return 0;
}
