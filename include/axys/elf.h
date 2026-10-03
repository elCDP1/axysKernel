#ifndef AXYS_ELF_H
#define AXYS_ELF_H

#include "axys/types.h"

/* Axys user executable ABI v1: ELF64, little-endian, x86-64, static ET_EXEC
 * or ET_DYN. The loader accepts at most 16 program headers and 64 MiB of
 * aggregate PT_LOAD memory. */
#define AXYS_ELF_ET_EXEC 2u
#define AXYS_ELF_ET_DYN 3u
#define AXYS_ELF_EM_X86_64 62u
#define AXYS_ELF_PT_LOAD 1u
#define AXYS_ELF_PT_DYNAMIC 2u
#define AXYS_ELF_PT_INTERP 3u
#define AXYS_ELF_PF_X 1u
#define AXYS_ELF_PF_W 2u
#define AXYS_ELF_MAX_PHDRS 16u
#define AXYS_ELF_MAX_IMAGE_MEMORY (64u * 1024u * 1024u)

struct axys_elf64_header {
    axys_uint8_t ident[16];
    axys_uint16_t type, machine;
    axys_uint32_t version;
    axys_uint64_t entry, phoff, shoff;
    axys_uint32_t flags;
    axys_uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct axys_elf64_phdr {
    axys_uint32_t type, flags;
    axys_uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

struct axys_elf_image_info {
    axys_uint64_t entry;
    axys_uint16_t type;
    axys_uint16_t phnum;
};

/* Validate all loader-relevant ELF metadata without mapping or executing it.
 * Returns 0 for the supported ABI and -1 for malformed/unsupported images. */
int axys_elf_validate_image(const void *image, axys_size_t size,
                            struct axys_elf_image_info *info);

#endif
