#include "axys/elf.h"
#include "axys/string.h"

#define ELF_HEADER_VERSION 1u
#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u
#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_PF_X 1u
#define ELF_PF_W 2u

AXYS_STATIC_ASSERT(sizeof(struct axys_elf64_header) == 64, elf64_header_size);
AXYS_STATIC_ASSERT(sizeof(struct axys_elf64_phdr) == 56, elf64_phdr_size);

static int range_inside(axys_uint64_t offset, axys_uint64_t length,
                        axys_uint64_t total)
{
    return offset <= total && length <= total - offset;
}

static int power_of_two(axys_uint64_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

int axys_elf_validate_image(const void *raw_image, axys_size_t size,
                            struct axys_elf_image_info *info)
{
    const axys_uint8_t *image = (const axys_uint8_t *)raw_image;
    struct axys_elf64_header eh;
    axys_uint64_t phbytes;
    axys_uint64_t memory_bytes = 0;
    axys_uint32_t load_count = 0;
    axys_uint32_t dynamic_count = 0;
    axys_uint64_t dynamic_vaddr = 0;
    axys_uint64_t dynamic_offset = 0;
    axys_uint64_t dynamic_filesz = 0;
    axys_uint64_t dynamic_memsz = 0;
    int entry_is_executable = 0;

    if (image == AXYS_NULL || info == AXYS_NULL || size < sizeof(eh)) {
        return -1;
    }
    axys_memcpy(&eh, image, sizeof(eh));
    if (eh.ident[0] != 0x7f || eh.ident[1] != 'E' || eh.ident[2] != 'L' ||
        eh.ident[3] != 'F' || eh.ident[4] != ELF_CLASS_64 ||
        eh.ident[5] != ELF_DATA_LSB || eh.ident[6] != ELF_HEADER_VERSION ||
        eh.machine != AXYS_ELF_EM_X86_64 || eh.version != ELF_HEADER_VERSION ||
        (eh.type != AXYS_ELF_ET_EXEC && eh.type != AXYS_ELF_ET_DYN) ||
        eh.ehsize != sizeof(eh) ||
        eh.phentsize != sizeof(struct axys_elf64_phdr) ||
        eh.phnum == 0 || eh.phnum > AXYS_ELF_MAX_PHDRS) {
        return -1;
    }
    phbytes = (axys_uint64_t)eh.phnum * sizeof(struct axys_elf64_phdr);
    if (!range_inside(eh.phoff, phbytes, size)) {
        return -1;
    }

    for (axys_uint16_t i = 0; i < eh.phnum; ++i) {
        struct axys_elf64_phdr ph;

        axys_memcpy(&ph, image + eh.phoff +
                              (axys_uint64_t)i * sizeof(ph), sizeof(ph));
        if (ph.type == ELF_PT_INTERP) {
            return -1; /* no dynamic linker or hosted runtime is implemented */
        }
        if (ph.type == ELF_PT_DYNAMIC) {
            ++dynamic_count;
            if (dynamic_count > 1 || ph.memsz == 0 || ph.filesz < 16 ||
                (ph.filesz & 15u) != 0 || ph.filesz > ph.memsz ||
                !range_inside(ph.offset, ph.filesz, size) ||
                ph.vaddr > ~(axys_uint64_t)0 - ph.memsz) {
                return -1;
            }
            dynamic_vaddr = ph.vaddr;
            dynamic_offset = ph.offset;
            dynamic_filesz = ph.filesz;
            dynamic_memsz = ph.memsz;
            continue;
        }
        if (ph.type != ELF_PT_LOAD) {
            continue;
        }

        ++load_count;
        if (ph.filesz > ph.memsz ||
            !range_inside(ph.offset, ph.filesz, size) ||
            ph.vaddr > ~(axys_uint64_t)0 - ph.memsz ||
            (ph.flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X) ||
            (ph.align > 1 &&
             (!power_of_two(ph.align) ||
              (ph.vaddr & (ph.align - 1u)) !=
                  (ph.offset & (ph.align - 1u))))) {
            return -1;
        }
        if (ph.memsz > AXYS_ELF_MAX_IMAGE_MEMORY - memory_bytes) {
            return -1;
        }
        memory_bytes += ph.memsz;
        if ((ph.flags & ELF_PF_X) != 0 && ph.filesz != 0 &&
            eh.entry >= ph.vaddr && eh.entry - ph.vaddr < ph.filesz) {
            entry_is_executable = 1;
        }
    }

    if (load_count == 0 || !entry_is_executable ||
        (dynamic_count != 0 && eh.type != AXYS_ELF_ET_DYN)) {
        return -1;
    }

    /* PT_DYNAMIC must live in memory supplied by a loadable segment. */
    if (dynamic_count != 0) {
        int dynamic_is_loaded = 0;

        for (axys_uint16_t i = 0; i < eh.phnum; ++i) {
            struct axys_elf64_phdr ph;
            axys_uint64_t load_end;

            axys_memcpy(&ph, image + eh.phoff +
                                  (axys_uint64_t)i * sizeof(ph), sizeof(ph));
            if (ph.type != ELF_PT_LOAD) {
                continue;
            }
            load_end = ph.vaddr + ph.memsz; /* checked in the first pass */
            if (dynamic_vaddr >= ph.vaddr && dynamic_vaddr <= load_end &&
                dynamic_memsz <= load_end - dynamic_vaddr &&
                dynamic_vaddr - ph.vaddr <= ph.filesz &&
                dynamic_filesz <= ph.filesz - (dynamic_vaddr - ph.vaddr) &&
                ph.offset <= ~(axys_uint64_t)0 - (dynamic_vaddr - ph.vaddr) &&
                dynamic_offset == ph.offset + (dynamic_vaddr - ph.vaddr)) {
                dynamic_is_loaded = 1;
                break;
            }
        }
        if (!dynamic_is_loaded) {
            return -1;
        }
    }
    info->entry = eh.entry;
    info->type = eh.type;
    info->phnum = eh.phnum;
    return 0;
}
