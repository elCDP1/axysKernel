#include "axys/acpi.h"

/* AML data objects used by the static _S5 package. Integer values are limited
 * to 0..7 because SLP_TYP is a three-bit field; wider AML integers outside that
 * range are rejected rather than truncated into a different power state. */
static int read_integer(const volatile axys_uint8_t *aml, axys_size_t end,
                        axys_size_t *cursor, axys_uint64_t *value)
{
    axys_uint8_t opcode;
    axys_size_t bytes;
    axys_uint64_t result = 0;

    if (*cursor >= end) {
        return 0;
    }
    opcode = aml[(*cursor)++];
    switch (opcode) {
    case 0x00: /* ZeroOp */
        *value = 0;
        return 1;
    case 0x01: /* OneOp */
        *value = 1;
        return 1;
    case 0xff: /* OnesOp */
        *value = ~(axys_uint64_t)0;
        return 1;
    case 0x0a: bytes = 1; break; /* BytePrefix */
    case 0x0b: bytes = 2; break; /* WordPrefix */
    case 0x0c: bytes = 4; break; /* DWordPrefix */
    case 0x0e: bytes = 8; break; /* QWordPrefix */
    default:
        return 0;
    }

    if (bytes > end - *cursor) {
        return 0;
    }
    for (axys_size_t i = 0; i < bytes; ++i) {
        result |= (axys_uint64_t)aml[(*cursor)++] << (i * 8u);
    }
    *value = result;
    return 1;
}

/* Decode the AML PkgLength and return the first byte after its package. AML's
 * encoded length includes its own length bytes, element count, and payload. */
static int package_end(const volatile axys_uint8_t *aml, axys_size_t length,
                       axys_size_t length_at, axys_size_t *contents_at,
                       axys_size_t *end_at)
{
    axys_size_t cursor = length_at;
    axys_uint8_t lead;
    axys_uint8_t following;
    axys_uint32_t package_length;

    if (cursor >= length) {
        return 0;
    }
    lead = aml[cursor++];
    following = (axys_uint8_t)(lead >> 6);
    if (following == 0) {
        package_length = lead & 0x3fu;
    } else {
        if ((lead & 0x30u) != 0u || (axys_size_t)following > length - cursor) {
            return 0;
        }
        package_length = lead & 0x0fu;
        for (axys_uint8_t i = 0; i < following; ++i) {
            package_length |= (axys_uint32_t)aml[cursor++] << (4u + 8u * i);
        }
    }

    if ((axys_size_t)package_length < cursor - length_at + 1u ||
        (axys_size_t)package_length > length - length_at) {
        return 0;
    }
    *contents_at = cursor;
    *end_at = length_at + package_length;
    return cursor < *end_at;
}

int axys_acpi_extract_s5(const volatile axys_uint8_t *aml, axys_size_t length,
                         axys_uint8_t *type_a, axys_uint8_t *type_b)
{
    if (aml == AXYS_NULL || type_a == AXYS_NULL || type_b == AXYS_NULL) {
        return -1;
    }

    /* _S5 is a root-namespace Name object. Firmware commonly emits the bare
     * NameSeg and may prefix it with RootChar; recognize both forms. */
    for (axys_size_t i = 0; i < length; ++i) {
        axys_size_t name = i + 1u;
        axys_size_t cursor;
        axys_size_t package_limit;
        axys_uint64_t a;
        axys_uint64_t b;

        if (aml[i] != 0x08u || name >= length) { /* NameOp */
            continue;
        }
        if (aml[name] == 0x5cu) { /* RootChar */
            ++name;
        }
        if (length - name < 5u || aml[name] != '_' || aml[name + 1u] != 'S' ||
            aml[name + 2u] != '5' || aml[name + 3u] != '_') {
            continue;
        }
        name += 4u;
        if (aml[name] != 0x12u) { /* PackageOp */
            continue;
        }
        if (!package_end(aml, length, name + 1u, &cursor, &package_limit) ||
            cursor >= package_limit || aml[cursor++] < 2u ||
            !read_integer(aml, package_limit, &cursor, &a) ||
            !read_integer(aml, package_limit, &cursor, &b) || a > 7u || b > 7u) {
            continue;
        }
        *type_a = (axys_uint8_t)a;
        *type_b = (axys_uint8_t)b;
        return 0;
    }
    return -1;
}
