#include "axys/cpu.h"
#include "axys/random.h"
#include "axys/spinlock.h"
#include "axys/string.h"

#define CHACHA_ROUNDS 20
#define CHACHA_BLOCK_WORDS 16
#define CHACHA_KEY_BYTES 32
#define CHACHA_BLOCK_BYTES 64

struct chacha_state {
    axys_uint32_t input[CHACHA_BLOCK_WORDS];
};

static struct chacha_state chacha;
static axys_size_t bytes_since_reseed;
static int random_ready;

/* ------------------------------------------------------------------ */
/* ChaCha20 core                                                       */
/* ------------------------------------------------------------------ */

static axys_uint32_t rotate_left(axys_uint32_t value, axys_uint32_t count)
{
    return (value << count) | (value >> (32u - count));
}

static axys_uint32_t load_le32(const axys_uint8_t *bytes)
{
    return (axys_uint32_t)bytes[0] | ((axys_uint32_t)bytes[1] << 8) |
           ((axys_uint32_t)bytes[2] << 16) | ((axys_uint32_t)bytes[3] << 24);
}

static void store_le32(axys_uint8_t *bytes, axys_uint32_t value)
{
    bytes[0] = (axys_uint8_t)(value & 0xffu);
    bytes[1] = (axys_uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (axys_uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (axys_uint8_t)((value >> 24) & 0xffu);
}

static void chacha_quarter_round(axys_uint32_t *state, unsigned a, unsigned b, unsigned c,
                                 unsigned d)
{
    state[a] += state[b];
    state[d] = rotate_left(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = rotate_left(state[b] ^ state[c], 12);
    state[a] += state[b];
    state[d] = rotate_left(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = rotate_left(state[b] ^ state[c], 7);
}

static void chacha_block(const axys_uint32_t input[CHACHA_BLOCK_WORDS],
                         axys_uint8_t output[CHACHA_BLOCK_BYTES])
{
    static const char sigma[17] = "expand 32-byte k";
    axys_uint32_t state[CHACHA_BLOCK_WORDS];
    unsigned i;

    for (i = 0; i < 4; ++i) {
        state[i] = load_le32((const axys_uint8_t *)sigma + i * 4);
    }
    for (i = 0; i < 8; ++i) {
        state[4 + i] = input[4 + i];
    }
    for (i = 12; i < CHACHA_BLOCK_WORDS; ++i) {
        state[i] = input[i];
    }

    for (i = 0; i < CHACHA_ROUNDS / 2; ++i) {
        chacha_quarter_round(state, 0, 4, 8, 12);
        chacha_quarter_round(state, 1, 5, 9, 13);
        chacha_quarter_round(state, 2, 6, 10, 14);
        chacha_quarter_round(state, 3, 7, 11, 15);
        chacha_quarter_round(state, 0, 5, 10, 15);
        chacha_quarter_round(state, 1, 6, 11, 12);
        chacha_quarter_round(state, 2, 7, 8, 13);
        chacha_quarter_round(state, 3, 4, 9, 14);
    }

    for (i = 0; i < CHACHA_BLOCK_WORDS; ++i) {
        store_le32(output + i * 4, state[i] + input[i]);
    }
}

static void chacha_init(const axys_uint8_t key[CHACHA_KEY_BYTES], axys_uint64_t nonce)
{
    static const char sigma[17] = "expand 32-byte k";
    unsigned i;

    for (i = 0; i < 4; ++i) {
        chacha.input[i] = load_le32((const axys_uint8_t *)sigma + i * 4);
    }
    for (i = 0; i < 8; ++i) {
        chacha.input[4 + i] = load_le32(key + i * 4);
    }
    /* Use the RFC 8439 state layout: word 12 is the block counter and
     * words 13..15 form the 96-bit nonce. We generate 64 random nonce bits
     * and keep the leading nonce word zero; the counter must remain separate
     * so chacha_output() can increment it without modifying the nonce. */
    chacha.input[12] = 0;
    chacha.input[13] = 0;
    chacha.input[14] = (axys_uint32_t)nonce;
    chacha.input[15] = (axys_uint32_t)(nonce >> 32);
}

/* Produce 64 fresh keystream bytes into `output`. The block is generated
 * straight into the caller's buffer; the `keystream` staging array only
 * exists to keep the block/counter bookkeeping in one place and is not part
 * of the output path. */
static void chacha_output(axys_uint8_t *output)
{
    chacha_block(chacha.input, output);
    ++chacha.input[12];
    if (chacha.input[12] == 0) {
        ++chacha.input[13];
    }
}

/* ------------------------------------------------------------------ */
/* Hardware entropy sources                                           */
/* ------------------------------------------------------------------ */

/* RDRAND reports success in CF. Hardware DRNG availability is checked before
 * executing the instruction so older CPUs never take an illegal-instruction
 * path during entropy collection. */
static int rdrand32(axys_uint32_t *value)
{
    unsigned attempt;

    if (value == AXYS_NULL || !axys_cpu_has_rdrand()) {
        return 0;
    }
    for (attempt = 0; attempt < 16; ++attempt) {
        unsigned char ok;

        __asm__ volatile("rdrand %0\n\tsetc %1"
                         : "=&r"(*value), "=&q"(ok)
                         :
                         : "cc");
        if (ok != 0) {
            return 1;
        }
    }
    return 0;
}

static int rdseed32(axys_uint32_t *value)
{
    unsigned attempt;

    if (value == AXYS_NULL || !axys_cpu_has_rdseed()) {
        return 0;
    }
    for (attempt = 0; attempt < 16; ++attempt) {
        unsigned char ok;

        __asm__ volatile("rdseed %0\n\tsetc %1"
                         : "=&r"(*value), "=&q"(ok)
                         :
                         : "cc");
        if (ok != 0) {
            return 1;
        }
    }
    return 0;
}

static axys_uint64_t read_tsc(void)
{
    axys_uint32_t low;
    axys_uint32_t high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((axys_uint64_t)high << 32) | low;
}

/* ------------------------------------------------------------------ */
/* Entropy and reseeding                                              */
/* ------------------------------------------------------------------ */

static void mix_tsc(axys_uint8_t *out, axys_size_t length)
{
    axys_size_t index;
    axys_uint64_t tsc = read_tsc();

    for (index = 0; index < length; ++index) {
        out[index] ^= (axys_uint8_t)(tsc >> ((index & 7u) * 8u));
    }
}

/* Timing-jitter fallback for CPUs without RDRAND/RDSEED (older hardware and
 * most default VMs). Each sample times a data-dependent memory walk with the
 * TSC and folds the delta into a persistent pool that is diffused through
 * ChaCha20. A health test rejects a TSC that does not vary. This is weaker
 * than a hardware DRNG, so the active source is always reported via
 * axys_random_source(). */
#define JITTER_FIRST_SAMPLES 8192u
#define JITTER_RESEED_SAMPLES 2048u
#define JITTER_MIN_DISTINCT 24u

static axys_uint8_t jitter_pool[CHACHA_KEY_BYTES];
static axys_uint8_t jitter_scratch[1024];
static axys_uint64_t irq_timing;
static int jitter_seeded;
static int entropy_source; /* AXYS_RANDOM_SOURCE_* */

void axys_random_add_timing(void)
{
    irq_timing = (irq_timing << 7) ^ (irq_timing >> 57) ^ read_tsc();
}

static void jitter_diffuse(axys_uint64_t tweak)
{
    axys_uint8_t block[CHACHA_BLOCK_BYTES];

    chacha_init(jitter_pool, tweak);
    chacha_output(block);
    axys_memcpy(jitter_pool, block, CHACHA_KEY_BYTES);
    axys_memset(block, 0, sizeof(block));
}

static int collect_jitter(axys_uint8_t *out, axys_size_t length)
{
    axys_uint8_t seen[256];
    unsigned distinct = 0;
    unsigned samples = jitter_seeded ? JITTER_RESEED_SAMPLES : JITTER_FIRST_SAMPLES;
    unsigned i;
    unsigned index = 0;
    axys_uint64_t previous = read_tsc();

    if (out == AXYS_NULL || length == 0 || length > CHACHA_BLOCK_BYTES) {
        return -1;
    }
    axys_memset(seen, 0, sizeof(seen));
    for (i = 0; i < samples; ++i) {
        axys_uint64_t now;
        axys_uint64_t delta;

        index = (index * 167u + jitter_pool[i & 31u] + 13u) & 1023u;
        jitter_scratch[index] = (axys_uint8_t)(jitter_scratch[index] + jitter_pool[(i + 7u) & 31u] + i);
        now = read_tsc();
        delta = now - previous;
        previous = now;
        if (!seen[delta & 0xffu]) {
            seen[delta & 0xffu] = 1;
            ++distinct;
        }
        jitter_pool[i & 31u] ^= (axys_uint8_t)(delta ^ (delta >> 8) ^ jitter_scratch[index]);
        if ((i & 63u) == 63u) {
            jitter_diffuse(delta ^ irq_timing);
        }
    }
    if (distinct < JITTER_MIN_DISTINCT) {
        return -1; /* TSC is not varying: refuse rather than fake entropy */
    }
    jitter_diffuse(irq_timing ^ previous);
    {
        /* Output comes from a separate ChaCha block so the pool itself is
         * never handed out directly. */
        axys_uint8_t block[CHACHA_BLOCK_BYTES];

        chacha_init(jitter_pool, previous ^ 0x6a6974746572ULL);
        chacha_output(block);
        axys_memcpy(out, block, length);
        axys_memset(block, 0, sizeof(block));
    }
    jitter_seeded = 1;
    return 0;
}

/* Prefer the hardware DRNG (TSC mixed in as extra diversity); when it is
 * absent or failing, fall back to the health-tested jitter collector. Never
 * returns weak data silently: the caller gets -1 if neither source is
 * healthy, and axys_random_source() reports which one was used. */
static int collect_entropy(axys_uint8_t *out, axys_size_t length)
{
    axys_size_t filled = 0;
    unsigned rounds = 0;

    if (out == AXYS_NULL || length == 0) {
        return -1;
    }
    if (axys_cpu_has_rdseed() || axys_cpu_has_rdrand()) {
        while (filled < length && rounds++ < 64u) {
            axys_uint32_t word;

            if (rdseed32(&word) || rdrand32(&word)) {
                axys_size_t chunk = length - filled;
                if (chunk > sizeof(word)) {
                    chunk = sizeof(word);
                }
                axys_memcpy(out + filled, &word, chunk);
                mix_tsc(out + filled, chunk);
                filled += chunk;
                word = 0;
            }
        }
        if (filled == length) {
            entropy_source = AXYS_RANDOM_SOURCE_HARDWARE;
            return 0;
        }
    }
    if (collect_jitter(out, length) == 0) {
        entropy_source = AXYS_RANDOM_SOURCE_JITTER;
        return 0;
    }
    entropy_source = AXYS_RANDOM_SOURCE_NONE;
    return -1;
}

int axys_random_source(void)
{
    return entropy_source;
}

static int random_reseed(void)
{
    axys_uint8_t material[CHACHA_KEY_BYTES + 8];
    axys_uint64_t nonce = 0;
    unsigned i;
    int status = collect_entropy(material, sizeof(material));

    if (status != 0) {
        axys_memset(material, 0, sizeof(material));
        return -1;
    }

    for (i = 0; i < 8; i += 4) {
        nonce |= (axys_uint64_t)load_le32(material + CHACHA_KEY_BYTES + i) << (i * 8);
    }
    chacha_init(material, nonce);
    axys_memset(material, 0, sizeof(material));
    bytes_since_reseed = 0;
    return 0;
}

/* One lock covers the ChaCha state, the reseed counter and the jitter pool.
 * Held with interrupts off; a reseed is bounded (a few thousand TSC samples). */
static struct axys_spinlock random_lock;

static int random_init_nl(void)
{
    random_ready = 0;
    bytes_since_reseed = 0;
    axys_memset(&chacha, 0, sizeof(chacha));
    if (random_reseed() != 0) {
        return -1;
    }
    random_ready = 1;
    return 0;
}

static int random_bytes_nl(void *buffer, axys_size_t length)
{
    axys_uint8_t *out = (axys_uint8_t *)buffer;
    axys_size_t offset = 0;
    axys_uint8_t block[CHACHA_BLOCK_BYTES];

    if (out == AXYS_NULL || length == 0) {
        return -1; /* a NULL/zero-length request is a caller bug, not output */
    }
    if (!random_ready && random_init_nl() != 0) {
        /* Entropy gathering failed. Refuse to emit predictable "random" data
         * rather than silently handing back a weak stream. */
        axys_memset(out, 0, length);
        return -1;
    }

    while (offset < length) {
        axys_size_t chunk = length - offset;

        if (bytes_since_reseed >= AXYS_RANDOM_RESEED_LIMIT) {
            if (random_reseed() != 0) {
                random_ready = 0;
                axys_memset(out + offset, 0, length - offset);
                axys_memset(block, 0, sizeof(block));
                return -1;
            }
        }

        chacha_output(block);
        if (chunk > sizeof(block)) {
            chunk = sizeof(block);
        }
        axys_memcpy(out + offset, block, chunk);
        offset += chunk;
        bytes_since_reseed += chunk;
    }
    axys_memset(block, 0, sizeof(block));
    return 0;
}

int axys_random_init(void)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&random_lock);
    int result = random_init_nl();

    axys_spin_unlock_irqrestore(&random_lock, flags);
    return result;
}

int axys_random_bytes(void *buffer, axys_size_t length)
{
    axys_uint64_t flags = axys_spin_lock_irqsave(&random_lock);
    int result = random_bytes_nl(buffer, length);

    axys_spin_unlock_irqrestore(&random_lock, flags);
    return result;
}

int axys_random_u32(axys_uint32_t *out)
{
    if (out == AXYS_NULL) {
        return -1;
    }
    *out = 0;
    return axys_random_bytes(out, sizeof(*out));
}

int axys_random_u64(axys_uint64_t *out)
{
    if (out == AXYS_NULL) {
        return -1;
    }
    *out = 0;
    return axys_random_bytes(out, sizeof(*out));
}

int axys_random_below(axys_uint64_t bound, axys_uint64_t *out)
{
    axys_uint64_t value;
    axys_uint64_t limit;

    if (out == AXYS_NULL) {
        return -1;
    }
    *out = 0;
    if (bound == 0) {
        return -1; /* documented precondition violation */
    }
    /* Rejection sampling removes the modulo bias that `value % bound` would
     * introduce when bound is not a power of two. */
    limit = ~0ULL - (~0ULL % bound) - 1;
    do {
        if (axys_random_u64(&value) != 0) {
            return -1;
        }
    } while (value > limit);
    *out = value % bound;
    return 0;
}
