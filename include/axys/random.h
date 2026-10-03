#ifndef AXYS_RANDOM_H
#define AXYS_RANDOM_H

#include "axys/types.h"

#define AXYS_RANDOM_RESEED_LIMIT 256

#define AXYS_RANDOM_SOURCE_NONE 0
#define AXYS_RANDOM_SOURCE_HARDWARE 1 /* RDSEED/RDRAND */
#define AXYS_RANDOM_SOURCE_JITTER 2   /* TSC timing-jitter fallback (weaker) */

/* Initialise the CSPRNG from RDSEED/RDRAND when present, otherwise from a
 * health-tested TSC timing-jitter collector. Returns 0 on success or -1 when
 * no healthy entropy source exists. */
int axys_random_init(void);

/* Fill `buffer` with `length` bytes. The generator reseeds from fresh hardware
 * entropy at AXYS_RANDOM_RESEED_LIMIT bytes. Returns 0 on success, or -1 when
 * entropy became unavailable mid-generation: the output is then zeroed so no
 * predictable stream escapes, but callers of security-sensitive values MUST
 * treat -1 as failure rather than reading the zeros as random data. */
int axys_random_bytes(void *buffer, axys_size_t length);

/* Which source last seeded the generator (AXYS_RANDOM_SOURCE_*). */
int axys_random_source(void);

/* Replace the stack-protector canary with a random value (see kernel/ssp.c).
 * Only call when no protected function frame is live. Returns 0 or -1. */
int axys_ssp_reseed(void);

/* Cheap hook for interrupt handlers: folds the TSC into the jitter pool.
 * Never blocks; safe to call from IRQ context with interrupts disabled. */
void axys_random_add_timing(void);

/* Convenience wrappers matching the shape of the C library/Linux APIs.
 * They return 0 on success and -1 on entropy failure; on failure `*out`
 * is left zeroed. Callers that cannot tolerate missing entropy must check
 * the return value instead of trusting the output bits. */
int axys_random_u32(axys_uint32_t *out);
int axys_random_u64(axys_uint64_t *out);

/* Uniform value in [0, bound) with modulo bias removed by rejection sampling.
 * `bound` must be non-zero. Returns 0 on success, -1 on entropy failure. */
int axys_random_below(axys_uint64_t bound, axys_uint64_t *out);

#endif
