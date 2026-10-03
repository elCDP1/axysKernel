#include <assert.h>
#include <stdio.h>

#include "axys/acpi.h"

static void test_bare_root_name(void)
{
    static const axys_uint8_t aml[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02,
        0x0a, 0x05, 0x0a, 0x05,
    };
    axys_uint8_t a = 0xff, b = 0xff;

    assert(axys_acpi_extract_s5(aml, sizeof(aml), &a, &b) == 0);
    assert(a == 5 && b == 5);
}

static void test_root_prefix_and_integer_encodings(void)
{
    static const axys_uint8_t aml[] = {
        0x08, 0x5c, '_', 'S', '5', '_', 0x12, 0x06, 0x02,
        0x00, 0x0b, 0x03, 0x00,
    };
    axys_uint8_t a = 0xff, b = 0xff;

    assert(axys_acpi_extract_s5(aml, sizeof(aml), &a, &b) == 0);
    assert(a == 0 && b == 3);
}

static void test_rejects_truncation_and_invalid_types(void)
{
    static const axys_uint8_t truncated[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x20, 0x02, 0x0a, 0x05,
    };
    static const axys_uint8_t invalid[] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02,
        0x0a, 0x08, 0x0a, 0x05,
    };
    axys_uint8_t a = 0xaa, b = 0xbb;

    assert(axys_acpi_extract_s5(truncated, sizeof(truncated), &a, &b) == -1);
    assert(axys_acpi_extract_s5(invalid, sizeof(invalid), &a, &b) == -1);
    assert(a == 0xaa && b == 0xbb);
}

int main(void)
{
    test_bare_root_name();
    test_root_prefix_and_integer_encodings();
    test_rejects_truncation_and_invalid_types();
    puts("test_acpi: ok");
    return 0;
}
