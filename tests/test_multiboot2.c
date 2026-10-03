#include <assert.h>
#include <stddef.h>

#include "axys/multiboot2.h"
#include "axys/string.h"

struct builder {
    axys_uint8_t *base;
    axys_size_t used;
    axys_size_t capacity;
};

static void put32(struct builder *b, axys_uint32_t value)
{
    assert(b->used + 4 <= b->capacity);
    axys_memcpy(b->base + b->used, &value, sizeof(value));
    b->used += 4;
}

static void put64(struct builder *b, axys_uint64_t value)
{
    assert(b->used + 8 <= b->capacity);
    axys_memcpy(b->base + b->used, &value, sizeof(value));
    b->used += 8;
}

static void pad_to_8(struct builder *b)
{
    while ((b->used & 7u) != 0) {
        assert(b->used + 1 <= b->capacity);
        b->base[b->used++] = 0;
    }
}

static void begin_tag(struct builder *b, axys_uint32_t type, axys_uint32_t size)
{
    put32(b, type);
    put32(b, size);
}

static void put_entry(struct builder *b, axys_uint64_t address,
                      axys_uint64_t length, axys_uint32_t type)
{
    put64(b, address);
    put64(b, length);
    put32(b, type);
    put32(b, 0);
}

static void finish(struct builder *b)
{
    axys_uint32_t total = (axys_uint32_t)b->used;
    axys_memcpy(b->base + 0, &total, sizeof(total));
    /* reserved field at offset 4 is already zero */
}

static void start_info(struct builder *b)
{
    b->used = AXYS_MB2_RUNTIME_INFO_BYTES;
    axys_memset(b->base, 0, b->capacity);
}

static void test_real_runtime_layout(void)
{
    _Alignas(8) axys_uint8_t buffer[512];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;
    const struct axys_mb2_mmap_entry *entry;
    const volatile axys_uint8_t *acpi_rsdp;

    start_info(&b);

    begin_tag(&b, AXYS_MULTIBOOT2_TAG_BASIC_MEMORY, 16);
    put32(&b, 640);
    put32(&b, 32768);
    pad_to_8(&b);

    /* Module tag: a bootloader-owned initrd must remain reserved until the
     * kernel explicitly consumes it. The command line is NUL-terminated. */
    begin_tag(&b, 3, 24);
    put32(&b, 0x220000);
    put32(&b, 0x240000);
    b.base[b.used++] = 'a';
    b.base[b.used++] = 'x';
    b.base[b.used++] = 'y';
    b.base[b.used++] = 's';
    b.base[b.used++] = '\0';
    while ((b.used & 7u) != 0) {
        b.base[b.used++] = 0;
    }

    /* Real Multiboot2 mmap tag: type + size + entry_size + entry_version,
     * followed immediately by the entry array. There is no entry-count word. */
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 2 * 24);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0x0, 0x9fc00, AXYS_MMAP_AVAILABLE);
    put_entry(&b, 0x100000, 0x7ee0000, AXYS_MMAP_AVAILABLE);
    pad_to_8(&b);

    /* Unknown tags must be skipped, not rejected merely because their type is
     * newer than this kernel knows. */
    begin_tag(&b, 99, 8);
    pad_to_8(&b);

    /* Framebuffer: address(8), pitch/width/height(12), bpp(1), type(1), pad(2). */
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_FRAMEBUFFER, 32);
    put64(&b, 0xe0000000ull);
    put32(&b, 1024);
    put32(&b, 320);
    put32(&b, 200);
    b.base[b.used++] = 32;
    b.base[b.used++] = 1;
    pad_to_8(&b);

    /* ACPI 2+ tag contains a copied 36-byte RSDP after its 8-byte header. */
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_ACPI_NEW, 44);
    acpi_rsdp = (const volatile axys_uint8_t *)(const void *)(b.base + b.used);
    for (axys_size_t i = 0; i < 36u; ++i) {
        b.base[b.used++] = (axys_uint8_t)i;
    }
    pad_to_8(&b);

    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);

    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_OK);
    assert(info.runtime_magic == AXYS_MULTIBOOT2_RUNTIME_MAGIC);
    assert(info.info_address == (axys_uintptr_t)(void *)buffer);
    assert(info.info_total_bytes == b.used);
    assert(info.mem_lower_kib == 640);
    assert(info.mem_upper_kib == 32768);
    assert(info.framebuffer_bpp == 32);
    assert(info.framebuffer_type == 1);
    assert(info.acpi_rsdp == acpi_rsdp);
    assert(info.acpi_rsdp_length == 36u);
    assert(info.module_count == 1);
    assert(info.modules[0].start == 0x220000);
    assert(info.modules[0].end == 0x240000);
    assert(axys_strcmp(info.modules[0].command_line, "axys") == 0);
    assert(info.mmap_entries == 2);
    assert(info.mmap_entry_size == sizeof(struct axys_mb2_mmap_entry));
    assert(info.mmap_entry_version == 0);

    entry = axys_mmap_entry(&info, 0);
    assert(entry != AXYS_NULL);
    assert(entry->address == 0);
    assert(entry->length == 0x9fc00);
    assert(entry->type == AXYS_MMAP_AVAILABLE);

    entry = axys_mmap_entry(&info, 1);
    assert(entry != AXYS_NULL);
    assert(entry->address == 0x100000);
    assert(entry->length == 0x7ee0000);

    assert(axys_mmap_entry(&info, 2) == AXYS_NULL);
    assert(axys_mmap_entry(&info, 9999) == AXYS_NULL);
    assert(axys_mmap_entry(AXYS_NULL, 0) == AXYS_NULL);
    assert(info.mem_available_bytes == 0x9fc00 + 0x7ee0000);
    assert(axys_mmap_highest_address(&info) == 0x7fe0000);
}

static void test_rejects_short_acpi_tag(void)
{
    _Alignas(8) axys_uint8_t buffer[256];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 40);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0x100000, 0x100000, AXYS_MMAP_AVAILABLE);
    pad_to_8(&b);

    begin_tag(&b, AXYS_MULTIBOOT2_TAG_ACPI_NEW, 43);
    for (axys_size_t i = 0; i < 35u; ++i) {
        b.base[b.used++] = 0;
    }
    pad_to_8(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);

    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);
}

static void test_extended_entry_stride(void)
{
    _Alignas(8) axys_uint8_t buffer[256];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 32);
    put32(&b, 32); /* future-compatible larger entry stride */
    put32(&b, 0);
    put_entry(&b, 0x100000, 0x200000, AXYS_MMAP_AVAILABLE);
    put64(&b, 0x1122334455667788ull); /* ignored future fields */
    pad_to_8(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);

    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_OK);
    assert(info.mmap_entries == 1);
    assert(info.mmap_entry_size == 32);
    assert(axys_mmap_entry(&info, 0)->address == 0x100000);
    assert(axys_mmap_entry(&info, 0)->length == 0x200000);
}

static void test_malformed_blocks_rejected(void)
{
    _Alignas(8) axys_uint8_t buffer[256];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 24);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0, 0x100000, AXYS_MMAP_AVAILABLE);
    /* Missing end tag. */
    finish(&b);
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);

    start_info(&b);
    put32(&b, 8); /* total_size says only the fixed header exists */
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_SIZE);

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 24);
    put32(&b, 20); /* entry size must be >= 24 and 8-byte aligned */
    put32(&b, 0);
    put_entry(&b, 0, 0x100000, AXYS_MMAP_AVAILABLE);
    pad_to_8(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);

    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 0, &info) == AXYS_MB2_ERR_MAGIC);
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)(buffer + 1),
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_ADDRESS);
    assert(axys_multiboot2_parse(0, AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_ADDRESS);
    assert(axys_multiboot2_parse(0x1000, AXYS_MULTIBOOT2_RUNTIME_MAGIC,
                                 AXYS_NULL) == AXYS_MB2_ERR_NULL);
}

static void test_invalid_entries_rejected(void)
{
    _Alignas(8) axys_uint8_t buffer[256];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 24);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0xfffffffffffff000ull, 0x2000, AXYS_MMAP_AVAILABLE);
    pad_to_8(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);

    start_info(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 16 + 24);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0, 0, AXYS_MMAP_AVAILABLE);
    pad_to_8(&b);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);
    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);
}

static void test_modules_rejected_when_unterminated(void)
{
    _Alignas(8) axys_uint8_t buffer[128];
    struct builder b = { buffer, 0, sizeof(buffer) };
    struct axys_boot_info info;

    start_info(&b);
    begin_tag(&b, 3, 24);
    put32(&b, 0x100000);
    put32(&b, 0x110000);
    for (int i = 0; i < 8; ++i) {
        b.base[b.used++] = 'x';
    }
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_MMAP, 40);
    put32(&b, 24);
    put32(&b, 0);
    put_entry(&b, 0, 0x100000, AXYS_MMAP_AVAILABLE);
    begin_tag(&b, AXYS_MULTIBOOT2_TAG_END, 8);
    finish(&b);

    assert(axys_multiboot2_parse((axys_uintptr_t)(void *)buffer,
                                 AXYS_MULTIBOOT2_RUNTIME_MAGIC, &info) ==
           AXYS_MB2_ERR_MALFORMED);
}

static void test_totals(void)
{
    struct axys_mb2_mmap_entry entries[2];
    struct axys_boot_info info;

    axys_memset(entries, 0, sizeof(entries));
    axys_memset(&info, 0, sizeof(info));
    entries[0].address = 0x100000;
    entries[0].length = 0x200000;
    entries[0].type = AXYS_MMAP_AVAILABLE;
    entries[1].address = 0xfffffffffffff000ull;
    entries[1].length = 0x2000;
    entries[1].type = AXYS_MMAP_AVAILABLE;
    info.mmap = entries;
    info.mmap_entries = 2;
    info.mmap_entry_size = sizeof(struct axys_mb2_mmap_entry);

    assert(axys_mmap_total_available(&info) == 0x200000);
    assert(axys_mmap_highest_address(&info) == 0x300000);
    assert(axys_mmap_total_available(AXYS_NULL) == 0);
    assert(axys_mmap_highest_address(AXYS_NULL) == 0);
}

int main(void)
{
    test_real_runtime_layout();
    test_rejects_short_acpi_tag();
    test_extended_entry_stride();
    test_malformed_blocks_rejected();
    test_invalid_entries_rejected();
    test_modules_rejected_when_unterminated();
    test_totals();
    return 0;
}
