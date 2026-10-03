#include <assert.h>
#include <string.h>

#include "axys/printf.h"
#include "axys/string.h"

static void test_string_primitives(void)
{
    char destination[32];
    char source[] = "axys";
    void *pointer = (void *)(axys_uintptr_t)0x1234;

    assert(axys_strlen("axys") == 4);
    assert(axys_strcmp("alpha", "alpha") == 0);
    assert(axys_strcmp("alpha", "beta") < 0);
    assert(axys_strncmp("alpha", "alphabet", 5) == 0);
    assert(axys_strlcpy(destination, source, sizeof(destination)) == 4);
    assert(axys_strcmp(destination, source) == 0);
    assert(axys_memcmp(source, destination, 5) == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%s-%d", "v", 7) == 3);
    assert(axys_strcmp(destination, "v-7") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%08x", 0x1234u) == 8);
    assert(axys_strcmp(destination, "00001234") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%llu", 1234567890123ull) == 13);
    assert(axys_strcmp(destination, "1234567890123") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%+05d", 42) == 5);
    assert(axys_strcmp(destination, "+0042") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%-5d|", 42) == 6);
    assert(axys_strcmp(destination, "42   |") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%#08x", 0x12u) == 8);
    assert(axys_strcmp(destination, "0x000012") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%p", pointer) == 18);
    assert(axys_strcmp(destination, "0x0000000000001234") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%zu", sizeof(destination)) == 2);
    assert(axys_strcmp(destination, "32") == 0);
    assert(axys_snprintf(destination, sizeof(destination), "%%") == 1);
    assert(axys_strcmp(destination, "%") == 0);
}

/*
 * A format string that ends immediately after '%' used to walk off the end of
 * the string. The default branch emitted the '%' *and* the NUL terminator,
 * inflating the reported length, and the unconditional `++format` then stepped
 * one byte past the terminator and kept formatting whatever followed the string
 * in memory. That is a silent out-of-bounds read: it can pick up live stack
 * data as conversion specifiers, and then consume varargs that were never
 * passed, so it leaks rather than crashes.
 *
 * The literal '%' must be kept -- a caller writing "50%" wants to see it -- but
 * nothing past the terminator may be consumed.
 */
static void test_trailing_percent(void)
{
    char destination[32];
    char guarded[32];

    assert(axys_snprintf(destination, sizeof(destination), "50%") == 3);
    assert(axys_strcmp(destination, "50%") == 0);

    /* Nothing after the '%' at all, so nothing can be over-read. */
    assert(axys_snprintf(destination, sizeof(destination), "%") == 1);
    assert(axys_strcmp(destination, "%") == 0);

    /*
     * A '%' followed only by flags. The flag, width and length loops all stop
     * on the terminator, so these used to reach the default branch with
     * *format == 0 and step past the end of the string.
     *
     * The flags themselves are consumed and then dropped, so the result is just
     * the literal '%'. That is a deliberate choice: a truncated conversion has
     * no argument to print, and echoing the flags back would be no more correct
     * -- the whole conversion is malformed. What matters is that the scan stops
     * at the terminator and reports a length the caller can slice with.
     */
    assert(axys_snprintf(destination, sizeof(destination), "%-") == 1);
    assert(axys_strcmp(destination, "%") == 0);

    assert(axys_snprintf(destination, sizeof(destination), "%#0123") == 1);
    assert(axys_strcmp(destination, "%") == 0);

    assert(axys_snprintf(destination, sizeof(destination), "%ll") == 1);
    assert(axys_strcmp(destination, "%") == 0);

    /*
     * The decisive case. The format string has to *end* at the '%' for the
     * over-read to be reachable, so the terminator goes in at index 4 -- and
     * then the bytes after it are deliberately filled with 'd', a conversion
     * specifier that would consume a vararg this call never passed.
     *
     * The old code emitted the '%' and the NUL itself and then advanced past
     * the terminator, so it read guarded[5] as a specifier, took an unpassed
     * argument for it, and kept going for the rest of the buffer. That shows up
     * here as a wrong length and a wrong string.
     */
    axys_memset(guarded, 0, sizeof(guarded));
    axys_memcpy(guarded, "100%", 4);
    axys_memset(guarded + 5, 'd', sizeof(guarded) - 5);

    assert(axys_snprintf(destination, sizeof(destination), guarded) == 4);
    assert(axys_strcmp(destination, "100%") == 0);

    /* Same string, entered at the '%' so there is nothing before it either. */
    assert(axys_snprintf(destination, sizeof(destination), guarded + 3) == 1);
    assert(axys_strcmp(destination, "%") == 0);

    /* A real unknown conversion keeps the old behaviour: echo it back. */
    assert(axys_snprintf(destination, sizeof(destination), "%q") == 2);
    assert(axys_strcmp(destination, "%q") == 0);
}

/*
 * axys_vprintf's 512-byte staging buffer must not be trusted past the value
 * axys_vsnprintf reports, and the reported length is what callers use to slice.
 * A format that exactly fills it must terminate rather than overrun.
 */
static void test_truncation(void)
{
    char destination[8];

    /* size 8 fits 7 characters plus the terminator. */
    assert(axys_snprintf(destination, sizeof(destination), "1234567") == 7);
    assert(axys_strcmp(destination, "1234567") == 0);

    assert(axys_snprintf(destination, sizeof(destination), "12345678") == 8);
    assert(strlen(destination) == sizeof(destination) - 1);

    /* A zero-size destination must still be legal and must not write. */
    assert(axys_snprintf(destination, 0, "ignored") == 7);
}

int main(void)
{
    test_string_primitives();
    test_trailing_percent();
    test_truncation();
    return 0;
}
