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

    /* boot.S splits the FIRST 2 MiB into 4 KiB pages (W^X for text/rodata);
     * everything past that -- .data, .bss, boot page tables and stacks -- is
     * covered by the 2 MiB huge-page PDEs of the low-GiB identity map, so the
     * real early-boot constraint is the whole first GiB, not 2 MiB. The
     * assertion still guards against a kernel so large it would collide with
     * GRUB-provided structures or exceed what the fixed boot tables cover. */
    ASSERT(__kernel_end <= 0x40000000, "kernel exceeds initial 1 GiB boot identity mapping")

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
