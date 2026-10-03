#ifndef AXYS_MULTIBOOT2_H
#define AXYS_MULTIBOOT2_H

#include "axys/types.h"

/* Magic embedded in the kernel image's Multiboot2 header. */
#define AXYS_MULTIBOOT2_HEADER_MAGIC 0xe85250d6u
/* Magic in EAX when a Multiboot2 bootloader transfers control to the kernel. */
#define AXYS_MULTIBOOT2_RUNTIME_MAGIC 0x36d76289u

#define AXYS_MULTIBOOT2_TAG_END 0u
#define AXYS_MULTIBOOT2_TAG_BASIC_MEMORY 4u
#define AXYS_MULTIBOOT2_TAG_MMAP 6u
#define AXYS_MULTIBOOT2_TAG_FRAMEBUFFER 8u
#define AXYS_MULTIBOOT2_TAG_ACPI_OLD 14u
#define AXYS_MULTIBOOT2_TAG_ACPI_NEW 15u
#define AXYS_MULTIBOOT2_MAX_INFO_SIZE (16u * 1024u * 1024u)
#define AXYS_MB2_MAX_MODULES 128u

#define AXYS_MMAP_AVAILABLE 1u
#define AXYS_MMAP_RESERVED 2u
#define AXYS_MMAP_ACPI_RECLAIMABLE 3u
#define AXYS_MMAP_ACPI_NVS 4u
#define AXYS_MMAP_BADRAM 5u

/* Fixed part at the start of the Multiboot2 boot-information structure. */
struct axys_mb2_runtime_info {
    axys_uint32_t total_size;
    axys_uint32_t reserved;
};

#define AXYS_MB2_RUNTIME_INFO_BYTES 8u

/* Every runtime information tag starts with this pair. */
struct axys_mb2_tag {
    axys_uint32_t type;
    axys_uint32_t size;
};

struct axys_mb2_basic_memory {
    axys_uint32_t lower;
    axys_uint32_t upper;
};

struct axys_mb2_mmap_entry {
    axys_uint64_t address;
    axys_uint64_t length;
    axys_uint32_t type;
    axys_uint32_t reserved;
};

struct axys_mb2_framebuffer {
    axys_uint64_t address;
    axys_uint32_t pitch;
    axys_uint32_t width;
    axys_uint32_t height;
    axys_uint8_t bpp;
    axys_uint8_t type;
    axys_uint16_t reserved;
};

struct axys_mb2_module {
    axys_uint32_t start;
    axys_uint32_t end;
    const char *command_line;
};

struct axys_boot_info {
    /* The runtime Multiboot2 hand-off magic. Kept separate from the image header
     * magic because they are distinct values with distinct meanings. */
    axys_uint32_t runtime_magic;
    axys_uintptr_t info_address;
    axys_uint32_t mem_lower_kib;
    axys_uint32_t mem_upper_kib;
    axys_uint64_t framebuffer_address;
    axys_uint32_t framebuffer_pitch;
    axys_uint32_t framebuffer_width;
    axys_uint32_t framebuffer_height;
    axys_uint32_t framebuffer_type;
    axys_uint32_t framebuffer_bpp;

    /* Optional Multiboot2 ACPI RSDP copy. It points into the bootloader's
     * information block, which remains reserved for the kernel lifetime. */
    const volatile axys_uint8_t *acpi_rsdp;
    axys_size_t acpi_rsdp_length;

    struct axys_mb2_module modules[AXYS_MB2_MAX_MODULES];
    axys_size_t module_count;

    /* The memory map points into the bootloader-owned information block and is
     * valid until the kernel overwrites that block. `mmap_entry_size` is the
     * firmware stride, which may be larger than our known 24-byte prefix. */
    const struct axys_mb2_mmap_entry *mmap;
    axys_size_t mmap_entries;
    axys_size_t mmap_entry_size;
    axys_uint32_t mmap_entry_version;

    /* Sum of the ranges marked available, used for diagnostics only. The PMM
     * computes actual free pages from the bitmap and does not trust this sum. */
    axys_uint64_t mem_available_bytes;

    /* Exact size of the runtime information block as reported by total_size. */
    axys_uint64_t info_total_bytes;
};

/* Runtime parser status. */
#define AXYS_MB2_OK 0
#define AXYS_MB2_ERR_NULL 1
#define AXYS_MB2_ERR_ADDRESS 2
#define AXYS_MB2_ERR_MAGIC 3
#define AXYS_MB2_ERR_SIZE 4
#define AXYS_MB2_ERR_NO_MMAP 5
#define AXYS_MB2_ERR_MALFORMED 6

/* Parse and validate a Multiboot2 runtime information block. The parser reads
 * the runtime magic supplied in EAX, then validates total_size and every tag
 * boundary before dereferencing tag payloads. */
int axys_multiboot2_parse(axys_uintptr_t address, axys_uint32_t runtime_magic,
                          struct axys_boot_info *boot_info);

/* Read mmap entry index from a parsed boot_info. */
const struct axys_mb2_mmap_entry *axys_mmap_entry(const struct axys_boot_info *info,
                                                  axys_size_t index);

/* Sum available memory ranges for diagnostics. Invalid/wrapping ranges are
 * ignored; the physical allocator separately validates and reserves frames. */
axys_uint64_t axys_mmap_total_available(const struct axys_boot_info *info);

/* Highest physical address described by the map, plus one. */
axys_uint64_t axys_mmap_highest_address(const struct axys_boot_info *info);

#endif
