#include "axys/multiboot2.h"
#include "axys/string.h"

#define AXYS_MB2_MMAP_FIXED_BYTES 16u
#define AXYS_MB2_MMAP_ENTRY_BYTES 24u

static int range_end(axys_uintptr_t base, axys_uintptr_t length,
                     axys_uintptr_t *end)
{
    if (length > ~(axys_uintptr_t)0 - base) {
        return 0;
    }
    *end = base + length;
    return 1;
}

static int axys_mb2_read_mmap(const struct axys_mb2_tag *tag,
                              struct axys_boot_info *boot_info)
{
    const axys_uint8_t *base = (const axys_uint8_t *)(const void *)tag;
    axys_uint32_t entry_size;
    axys_uint32_t entry_version;
    axys_size_t payload_bytes;
    axys_size_t count;
    axys_size_t index;

    if (tag->size < AXYS_MB2_MMAP_FIXED_BYTES) {
        return 0;
    }

    axys_memcpy(&entry_size, base + 8, sizeof(entry_size));
    axys_memcpy(&entry_version, base + 12, sizeof(entry_version));

    /* Future versions are specified to extend the entry, but an unknown
     * version is not safe to interpret here. */
    if (entry_version != 0 || entry_size < AXYS_MB2_MMAP_ENTRY_BYTES ||
        (entry_size & 7u) != 0) {
        return 0;
    }

    payload_bytes = (axys_size_t)tag->size - AXYS_MB2_MMAP_FIXED_BYTES;
    if (payload_bytes == 0 || payload_bytes % entry_size != 0) {
        return 0;
    }
    count = payload_bytes / entry_size;
    if (count == 0) {
        return 0;
    }

    /* Validate every entry before exposing the array through boot_info. */
    for (index = 0; index < count; ++index) {
        const axys_uint8_t *raw = base + AXYS_MB2_MMAP_FIXED_BYTES +
                                  index * entry_size;
        axys_uint64_t address;
        axys_uint64_t length;
        axys_uint64_t end;

        axys_memcpy(&address, raw, sizeof(address));
        axys_memcpy(&length, raw + 8, sizeof(length));
        if (length == 0 || address + length < address) {
            return 0;
        }
        end = address + length;
        (void)end;
    }

    boot_info->mmap = (const struct axys_mb2_mmap_entry *)(const void *)
        (base + AXYS_MB2_MMAP_FIXED_BYTES);
    boot_info->mmap_entries = count;
    boot_info->mmap_entry_size = entry_size;
    boot_info->mmap_entry_version = entry_version;
    return 1;
}

static int axys_mb2_walk_tags(axys_uintptr_t cursor, axys_uintptr_t end,
                              struct axys_boot_info *boot_info)
{
    /* 1 = success, 0 = malformed block, -1 = structurally valid but no mmap. */
    while (cursor < end) {
        axys_uint32_t tag_type;
        axys_uint32_t tag_size;
        axys_uintptr_t remaining = end - cursor;
        axys_uintptr_t padded_size;
        const struct axys_mb2_tag *tag;

        if (remaining < sizeof(struct axys_mb2_tag)) {
            return 0;
        }

        tag = (const struct axys_mb2_tag *)(const void *)cursor;
        axys_memcpy(&tag_type, (const void *)(axys_uintptr_t)cursor, sizeof(tag_type));
        axys_memcpy(&tag_size, (const void *)(axys_uintptr_t)(cursor + 4), sizeof(tag_size));

        if (tag_size < sizeof(struct axys_mb2_tag) ||
            (axys_uintptr_t)tag_size > remaining) {
            return 0;
        }
        padded_size = ((axys_uintptr_t)tag_size + 7u) & ~(axys_uintptr_t)7u;
        if (padded_size > remaining) {
            return 0;
        }

        if (tag_type == AXYS_MULTIBOOT2_TAG_END) {
            /* The end tag is exactly 8 bytes and must terminate the block. */
            if (tag_size != 8u || padded_size != remaining) {
                return 0;
            }
            return boot_info->mmap != AXYS_NULL ? 1 : -1;
        }

        switch (tag_type) {
        case AXYS_MULTIBOOT2_TAG_BASIC_MEMORY:
            if (tag_size >= 16u) {
                axys_memcpy(&boot_info->mem_lower_kib, (const void *)(axys_uintptr_t)(cursor + 8), 4);
                axys_memcpy(&boot_info->mem_upper_kib, (const void *)(axys_uintptr_t)(cursor + 12), 4);
            }
            break;

        case AXYS_MULTIBOOT2_TAG_MMAP:
            if (boot_info->mmap != AXYS_NULL ||
                !axys_mb2_read_mmap(tag, boot_info)) {
                return 0;
            }
            break;

        case 3u: /* module: start, end, command line */
            if (tag_size < 16u || boot_info->module_count >= AXYS_MB2_MAX_MODULES) {
                return 0;
            }
            {
                axys_uint32_t start;
                axys_uint32_t module_end;
                axys_size_t string_offset = 16u;
                axys_size_t string_length = 0;

                axys_memcpy(&start, (const void *)(axys_uintptr_t)(cursor + 8), 4);
                axys_memcpy(&module_end, (const void *)(axys_uintptr_t)(cursor + 12), 4);
                if (module_end <= start) {
                    return 0;
                }
                while (string_offset + string_length < tag_size &&
                       *(const axys_uint8_t *)(cursor + string_offset + string_length) != 0) {
                    ++string_length;
                }
                if (string_offset + string_length >= tag_size) {
                    return 0; /* command line must be NUL-terminated in the tag */
                }
                boot_info->modules[boot_info->module_count].start = start;
                boot_info->modules[boot_info->module_count].end = module_end;
                boot_info->modules[boot_info->module_count].command_line =
                    (const char *)(const void *)(cursor + string_offset);
                ++boot_info->module_count;
            }
            break;

        case AXYS_MULTIBOOT2_TAG_FRAMEBUFFER:
            if (tag_size < 32u) {
                return 0;
            }
            /* tag+8..+29 is the common framebuffer prefix. */
            axys_memcpy(&boot_info->framebuffer_address,
                        (const void *)(axys_uintptr_t)(cursor + 8), 8);
            axys_memcpy(&boot_info->framebuffer_pitch,
                        (const void *)(axys_uintptr_t)(cursor + 16), 4);
            axys_memcpy(&boot_info->framebuffer_width,
                        (const void *)(axys_uintptr_t)(cursor + 20), 4);
            axys_memcpy(&boot_info->framebuffer_height,
                        (const void *)(axys_uintptr_t)(cursor + 24), 4);
            boot_info->framebuffer_bpp = *(const axys_uint8_t *)(cursor + 28);
            boot_info->framebuffer_type = *(const axys_uint8_t *)(cursor + 29);
            break;

        case AXYS_MULTIBOOT2_TAG_ACPI_OLD:
            /* The old RSDP contains the 20-byte ACPI 1.0 prefix. Prefer a
             * later ACPI 2+ tag if the loader supplies both. */
            if (tag_size < 28u) {
                return 0;
            }
            if (boot_info->acpi_rsdp == AXYS_NULL) {
                boot_info->acpi_rsdp =
                    (const volatile axys_uint8_t *)(const void *)(cursor + 8u);
                boot_info->acpi_rsdp_length = tag_size - 8u;
            }
            break;

        case AXYS_MULTIBOOT2_TAG_ACPI_NEW:
            /* A new RSDP includes the extended 36-byte structure; ACPI checks
             * the embedded length and both checksums before following it. */
            if (tag_size < 44u) {
                return 0;
            }
            boot_info->acpi_rsdp =
                (const volatile axys_uint8_t *)(const void *)(cursor + 8u);
            boot_info->acpi_rsdp_length = tag_size - 8u;
            break;

        default:
            /* Unknown tags are allowed by the extensible Multiboot2 format. */
            break;
        }

        cursor += padded_size;
    }

    return 0;
}

const struct axys_mb2_mmap_entry *axys_mmap_entry(const struct axys_boot_info *info,
                                                  axys_size_t index)
{
    const axys_uint8_t *base;

    if (info == AXYS_NULL || info->mmap == AXYS_NULL ||
        index >= info->mmap_entries || info->mmap_entry_size < sizeof(struct axys_mb2_mmap_entry)) {
        return AXYS_NULL;
    }

    if (index > ((~(axys_size_t)0) / info->mmap_entry_size)) {
        return AXYS_NULL;
    }
    base = (const axys_uint8_t *)(const void *)info->mmap;
    return (const struct axys_mb2_mmap_entry *)(const void *)
        (base + index * info->mmap_entry_size);
}

int axys_multiboot2_parse(axys_uintptr_t address, axys_uint32_t runtime_magic,
                          struct axys_boot_info *boot_info)
{
    const struct axys_mb2_runtime_info *runtime;
    axys_uint32_t total_size;
    axys_uintptr_t end;

    if (boot_info == AXYS_NULL) {
        return AXYS_MB2_ERR_NULL;
    }
    axys_memset(boot_info, 0, sizeof(*boot_info));

    if (runtime_magic != AXYS_MULTIBOOT2_RUNTIME_MAGIC) {
        return AXYS_MB2_ERR_MAGIC;
    }
    if (address == 0 || (address & 7u) != 0) {
        return AXYS_MB2_ERR_ADDRESS;
    }

    runtime = (const struct axys_mb2_runtime_info *)(const void *)address;
    axys_memcpy(&total_size, (const void *)(axys_uintptr_t)address, sizeof(total_size));

    if (total_size < AXYS_MB2_RUNTIME_INFO_BYTES + 8u ||
        total_size > AXYS_MULTIBOOT2_MAX_INFO_SIZE ||
        (total_size & 7u) != 0 ||
        !range_end(address, (axys_uintptr_t)total_size, &end)) {
        return AXYS_MB2_ERR_SIZE;
    }

    (void)runtime;
    boot_info->runtime_magic = runtime_magic;
    boot_info->info_address = address;
    {
        int walk_result = axys_mb2_walk_tags(address + AXYS_MB2_RUNTIME_INFO_BYTES, end,
                                             boot_info);
        if (walk_result != 1) {
            axys_memset(boot_info, 0, sizeof(*boot_info));
            return walk_result < 0 ? AXYS_MB2_ERR_NO_MMAP : AXYS_MB2_ERR_MALFORMED;
        }
    }

    boot_info->mem_available_bytes = axys_mmap_total_available(boot_info);
    boot_info->info_total_bytes = total_size;
    return AXYS_MB2_OK;
}

axys_uint64_t axys_mmap_total_available(const struct axys_boot_info *info)
{
    axys_uint64_t total = 0;
    axys_size_t index;

    if (info == AXYS_NULL || info->mmap == AXYS_NULL) {
        return 0;
    }

    for (index = 0; index < info->mmap_entries; ++index) {
        const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);
        axys_uint64_t end;
        axys_uint64_t length;

        if (entry == AXYS_NULL) {
            break;
        }
        if (entry->type != AXYS_MMAP_AVAILABLE ||
            entry->length == 0 || entry->address + entry->length < entry->address) {
            continue;
        }
        end = entry->address + entry->length;
        length = end - entry->address;
        if (~(axys_uint64_t)0 - total < length) {
            return ~(axys_uint64_t)0;
        }
        total += length;
    }

    return total;
}

axys_uint64_t axys_mmap_highest_address(const struct axys_boot_info *info)
{
    axys_uint64_t highest = 0;
    axys_size_t index;

    if (info == AXYS_NULL || info->mmap == AXYS_NULL) {
        return 0;
    }

    for (index = 0; index < info->mmap_entries; ++index) {
        const struct axys_mb2_mmap_entry *entry = axys_mmap_entry(info, index);
        axys_uint64_t end;

        if (entry == AXYS_NULL || entry->length == 0 ||
            entry->address + entry->length < entry->address) {
            continue;
        }
        end = entry->address + entry->length;
        if (entry->type == AXYS_MMAP_AVAILABLE && end > highest) {
            highest = end;
        }
    }

    return highest;
}
