#include <assert.h>
#include <stdio.h>

#include "axys/elf.h"
#include "axys/string.h"

#define IMAGE_BYTES 8192u
#define PH_OFFSET sizeof(struct axys_elf64_header)
#define TEXT_OFFSET 4096u

_Alignas(8) static axys_uint8_t image[IMAGE_BYTES];

static struct axys_elf64_header *header(void)
{
    return (struct axys_elf64_header *)(void *)image;
}

static struct axys_elf64_phdr *program(unsigned index)
{
    return (struct axys_elf64_phdr *)(void *)(image + PH_OFFSET +
             index * sizeof(struct axys_elf64_phdr));
}

static void make_valid(void)
{
    axys_memset(image, 0, sizeof(image));
    header()->ident[0] = 0x7f;
    header()->ident[1] = 'E';
    header()->ident[2] = 'L';
    header()->ident[3] = 'F';
    header()->ident[4] = 2;
    header()->ident[5] = 1;
    header()->ident[6] = 1;
    header()->type = AXYS_ELF_ET_DYN;
    header()->machine = AXYS_ELF_EM_X86_64;
    header()->version = 1;
    header()->entry = 0x1000;
    header()->phoff = PH_OFFSET;
    header()->ehsize = sizeof(struct axys_elf64_header);
    header()->phentsize = sizeof(struct axys_elf64_phdr);
    header()->phnum = 1;

    program(0)->type = AXYS_ELF_PT_LOAD;
    program(0)->flags = AXYS_ELF_PF_X;
    program(0)->offset = TEXT_OFFSET;
    program(0)->vaddr = 0x1000;
    program(0)->filesz = 1;
    program(0)->memsz = 1;
    program(0)->align = 4096;
    image[TEXT_OFFSET] = 0xc3; /* ret */
}

static void test_valid_static_pie(void)
{
    struct axys_elf_image_info info;

    make_valid();
    assert(axys_elf_validate_image(image, sizeof(image), &info) == 0);
    assert(info.entry == 0x1000);
    assert(info.type == AXYS_ELF_ET_DYN);
    assert(info.phnum == 1);
}

static void test_rejects_bad_header_and_truncation(void)
{
    struct axys_elf_image_info info;

    make_valid();
    header()->machine = 3;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    assert(axys_elf_validate_image(image, PH_OFFSET + sizeof(*program(0)) - 1,
                                   &info) != 0);

    make_valid();
    header()->phnum = AXYS_ELF_MAX_PHDRS + 1;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);
}

static void test_rejects_bad_segment_bounds_and_flags(void)
{
    struct axys_elf_image_info info;

    make_valid();
    program(0)->filesz = 2;
    program(0)->memsz = 1;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->offset = ~(axys_uint64_t)0;
    program(0)->filesz = 1;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->vaddr = ~(axys_uint64_t)0 - 15;
    program(0)->memsz = 32;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->flags = AXYS_ELF_PF_W | AXYS_ELF_PF_X;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->align = 3;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->offset = TEXT_OFFSET + 1;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    program(0)->memsz = AXYS_ELF_MAX_IMAGE_MEMORY + 1u;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);
}

static void test_rejects_aggregate_memory_over_limit(void)
{
    struct axys_elf_image_info info;

    make_valid();
    header()->phnum = 2;
    program(0)->memsz = 40u * 1024u * 1024u;
    program(0)->filesz = 1;
    program(1)->type = AXYS_ELF_PT_LOAD;
    program(1)->flags = AXYS_ELF_PF_W;
    program(1)->offset = 0;
    program(1)->vaddr = 0x2000000;
    program(1)->memsz = 40u * 1024u * 1024u;
    program(1)->align = 4096;

    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);
}

static void test_entry_and_dynamic_segment_policy(void)
{
    struct axys_elf_image_info info;

    make_valid();
    program(0)->flags = 0;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    header()->phnum = 2;
    program(1)->type = AXYS_ELF_PT_INTERP;
    program(1)->offset = TEXT_OFFSET;
    program(1)->filesz = 1;
    program(1)->memsz = 1;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    header()->type = AXYS_ELF_ET_EXEC;
    header()->phnum = 2;
    program(0)->memsz = 32;
    program(0)->filesz = 32;
    program(1)->type = AXYS_ELF_PT_DYNAMIC;
    program(1)->offset = TEXT_OFFSET;
    program(1)->vaddr = 0x1000;
    program(1)->filesz = 16;
    program(1)->memsz = 16;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);

    make_valid();
    header()->phnum = 2;
    program(0)->memsz = 32;
    program(0)->filesz = 32;
    program(1)->type = AXYS_ELF_PT_DYNAMIC;
    program(1)->offset = TEXT_OFFSET;
    program(1)->vaddr = 0x1000;
    program(1)->filesz = 16;
    program(1)->memsz = 16;
    assert(axys_elf_validate_image(image, sizeof(image), &info) == 0);

    program(1)->vaddr = 0x2000;
    assert(axys_elf_validate_image(image, sizeof(image), &info) != 0);
}

int main(void)
{
    test_valid_static_pie();
    test_rejects_bad_header_and_truncation();
    test_rejects_bad_segment_bounds_and_flags();
    test_rejects_aggregate_memory_over_limit();
    test_entry_and_dynamic_segment_policy();
    puts("test_elf: ok");
    return 0;
}
