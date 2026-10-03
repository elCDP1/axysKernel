OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* The kernel is loaded by GRUB at the 1 MiB mark and entered in 32-bit
 * protected mode. Everything the kernel touches during early boot must
 * therefore live in the low 1 GiB, because the first page table maps that
 * range with 2 MiB pages. */
PHDRS
{
    text   PT_LOAD FLAGS(5); /* R + X */
    rodata PT_LOAD FLAGS(4); /* R     */
    data   PT_LOAD FLAGS(6); /* R + W */
}

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .text ALIGN(4K) :
    {
        __text_start = .;
        /* GRUB locates the kernel by scanning for the multiboot2 magic in
         * the first 32768 bytes of the file (8-byte aligned), so this must
         * never be collected. */
        KEEP(*(.multiboot))
        *(.text .text.*)
        __text_end = .;
    } :text

    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } :data

    .bss ALIGN(4K) :
    {
        /* The linker synthesises __bss_start/__bss_end and the boot code
         * must zero this range before trusting any C global. */
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.* .lbss .lbss.* .sbss .sbss.*)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    . = ALIGN(4K);
    __kernel_end = .;

    /* boot.S only maps the kernel's initial 2 MiB with 4 KiB pages. Refuse a
     * link that grows beyond that contract instead of producing an image that
     * boots and then faults when it executes/copies late sections. */
    ASSERT(__kernel_end <= 0x200000, "kernel exceeds initial 2 MiB boot mapping")

    /*
     * Only truly inert sections are discarded. Debug and note sections are
     * harmless and useful, so they are left alone; in particular nothing that
     * --gc-sections needs to anchor on may be dropped here.
     */
    /DISCARD/ :
    {
        *(.eh_frame .eh_frame_hdr)
    }
}
