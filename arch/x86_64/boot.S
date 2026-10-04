.section .multiboot
.align 8
/*
 * Multiboot2 kernel-image header. This is NOT the runtime boot-information
 * structure passed in EBX. The header requests a memory-map tag from the
 * bootloader; the runtime block itself begins with total_size/reserved.
 *
 * Layout: 16-byte fixed header + 24-byte information-request tag + 8-byte end
 * tag = 48 bytes.
 */
mb_header:
    .long 0xe85250d6            /* Multiboot2 image-header magic */
    .long 0                     /* architecture: i386 */
    .long 48                    /* total header length */
    .long (0x100000000 - (0xe85250d6 + 48)) /* checksum: sum of first four words == 0 */

    /* Request basic memory, mmap, framebuffer, and (when published) ACPI RSDP. */
    .short 1                    /* header tag type: information request */
    .short 1                    /* optional: boot even if a requested tag is absent */
    .long 24                    /* tag size: 8-byte header + 4 u32 requests */
    .long 4                     /* basic memory information */
    .long 6                     /* memory map */
    .long 8                     /* framebuffer information */
    .long 15                    /* ACPI 2+ RSDP, when firmware/loader provide it */

    .short 0                    /* end tag */
    .short 0
    .long 8

.section .text
.code32
.global _start

#if CONFIG_AXYS_CONSOLE_SERIAL
.macro DBG c
    movb $\c, %al
    call serial_putc
.endm
#else
.macro DBG c
.endm
#endif

_start:
    cli
    cld
    mov $stack_top, %esp
    xor %ebp, %ebp

    /*
     * Zero .bss before anything can depend on a global being zero-initialised.
     * The Multiboot2 specification only requires the loader to zero the parts of
     * memory not covered by a load segment, so the kernel must not assume the
     * loader did this. Nothing stored here is in .bss, and rep stosb only
     * clobbers EAX/ECX/EDI, so the two loader words are parked in EDX/ESI to
     * survive the clear.
     */
    movl %eax, %edx
    movl %ebx, %esi
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xor %eax, %eax
    rep stosb

    movl %edx, mb_magic
    movl %esi, mb_info

#if CONFIG_AXYS_CONSOLE_SERIAL
    movb $'A', %al
    call serial_putc
#endif
    DBG 'B'

    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb boot_halt
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz boot_halt
    DBG 'C'

    /* Enable EFER.LME. NXE is enabled only when CPUID advertises NX. */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    pushl %eax
    mov $0x80000001, %eax
    cpuid
    popl %eax
    test $(1 << 20), %edx
    jz boot_halt               /* page tables below require architectural NX */
    or $0x800, %eax            /* EFER.NXE */
    mov $0xC0000080, %ecx
    wrmsr

    /*
     * PAE (bit 5) is required for long mode. OSFXSR (bit 9) must also be set or
     * executing SSE/SSE2 (e.g. vector/SIMD code in the kernel) raises #UD. OSFXSR can
     * only be set while CR0.EM = 0, which is already the case here.
     */
    mov %cr4, %eax
    or $0x20, %eax     /* CR4.PAE */
    or $0x200, %eax    /* CR4.OSFXSR */
    pushl %eax          /* new CR4 value; CPUID below clobbers EAX/EBX/ECX/EDX */

    /*
     * CR4.SMEP (bit 20) stops ring 0 from executing pages marked user-accessible.
     * It must only be written when the CPU advertises support: on a model that
     * leaves the bit reserved the write raises #GP, and at this point in boot no
     * IDT is installed, so the fault would escalate straight to a triple fault.
     * CPUID.7.0:EBX bit 7 is the architectural SMEP feature bit.
     */
    mov $0, %eax
    cpuid
    cmp $7, %eax
    jb 1f
    mov $7, %eax
    xor %ecx, %ecx
    cpuid
    test $(1 << 7), %ebx
    jz 1f
    popl %eax
    or $0x100000, %eax
    jmp 2f
1:
    popl %eax
2:
    mov %eax, %cr4
    DBG '1'

    call setup_page_tables
    DBG 'E'

    lgdt gdt64_pointer

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    DBG 'F'

    /*
     * CR0 bits: clear EM (bit 4) so the FPU/SSE is usable, set WP (bit 16) so
     * supervisor writes honour page read-only bits, then set PG|PE.
     * CR0.WP is what makes a read-only page actually read-only for ring 0;
     * without it the W^X guarantee from the NX bit is only advisory.
     *
     * Nothing may touch the stack between this store and the far jump below:
     * until CS.L is set the CPU still uses a legacy/PAE 32-bit walk, and the
     * stack lives outside the identity range this table maps through that view.
     */
    mov %cr0, %eax
    and $0xFFFFFFEF, %eax
    or $0x80010001, %eax   /* PG | WP | PE */
    mov %eax, %ecx
    mov %ecx, %cr0

    .byte 0xea
    .long long_mode_entry
    .word 0x08

boot_halt:
    cli
    hlt
    jmp boot_halt

setup_page_tables:
    /*
     * Long mode reads 8-byte paging entries, so every table below is filled
     * at an 8-byte stride with the upper half of each entry zeroed. Filling at
     * a 4-byte stride would make an entry's upper dword equal to the *next*
     * entry, pushing bits into the reserved region above bit 51 and faulting
     * with error code 0x8.
     */
    mov $pml4_table, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pdpt_table, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pd_table0, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pd_table1, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pd_table2, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pd_table3, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    mov $pt_low_table, %edi
    xor %eax, %eax
    mov $1024, %ecx
    rep stosl

    /* PML4[0] = pdpt_table | 0x3 */
    mov $pml4_table, %edi
    mov $pdpt_table, %eax
    or $0x3, %eax
    movl %eax, 0(%edi)
    movl $0, 4(%edi)

    /* PDPT[0] = pd_table0 | 0x3 */
    mov $pdpt_table, %edi
    mov $pd_table0, %eax
    or $0x3, %eax
    movl %eax, 0(%edi)
    movl $0, 4(%edi)

    /*
     * Split the first 2 MiB into 4 KiB pages so the kernel image can get real
     * W^X permissions even though the rest of the identity map remains backed
     * by cheap 2 MiB pages. Text is read/execute, rodata is read-only/NX, and
     * data/BSS are read/write/NX. Pages above it are also marked NX below.
     */
    mov $pt_low_table, %edi
    xor %eax, %eax                 /* physical page base */
    mov $512, %ecx
pgt_low:
    /*
     * NX/XD is bit 63 of the entry. In this split-dword write scheme that is
     * bit 31 of the HIGH dword only. Putting 0x80000000 in the low dword
     * would not set NX: it would add 2 GiB to the physical address and map
     * every page to X + 0x80000000 instead of X -- fatal for .data accesses
     * (mb_magic/mb_info) right after long-mode entry. Low half therefore
     * carries only P/W/U-style flags; high dword carries NX.
     */
    mov $0x00000003, %ebx          /* default: present | writable */
    mov $0x80000000, %edx          /* NX in the upper dword (bit 63) */
    cmpl $__text_start, %eax
    jb 1f
    cmpl $__text_end, %eax
    jae 1f
    mov $0x1, %ebx                 /* text: present, supervisor, read/execute */
    xor %edx, %edx
    jmp 3f
1:
    cmpl $__rodata_start, %eax
    jb 3f
    cmpl $__rodata_end, %eax
    jae 3f
    mov $0x00000001, %ebx          /* rodata: present, supervisor, read-only (NX stays in the high dword) */
3:
    /* Page 0 (NULL dereferences must fault) and the stack guard page below
     * stack_bottom stay NOT present: a stray access there takes a #PF
     * immediately instead of silently reading/writing real memory. */
    test %eax, %eax
    jz 4f
    cmpl $stack_guard, %eax
    jne 5f
4:
    xor %ebx, %ebx
    xor %edx, %edx
5:
    movl %eax, 0(%edi)
    orl %ebx, 0(%edi)
    movl %edx, 4(%edi)
    add $8, %edi
    add $0x1000, %eax
    dec %ecx
    jnz pgt_low

    /* PDE 0 now points at the fine-grained first page table. */
    mov $pd_table0, %edi
    mov $pt_low_table, %eax
    or $0x3, %eax
    movl %eax, 0(%edi)
    movl $0, 4(%edi)

    /* The 4 KiB table above covers exactly one 2 MiB region; every kernel
     * page beyond it (including .rodata/.data/.bss and the boot stacks,
     * which live past __kernel_end's text+rodata prefix) is mapped by the
     * remaining huge-page PDEs below. */
    add $8, %edi
    mov $0x00200083, %eax          /* second 2 MiB page: phys 0x200000 | 0x83 */
    mov $511, %ecx
pgt_pd:
    movl %eax, 0(%edi)
    movl $0x80000000, 4(%edi)
    add $8, %edi
    add $0x200000, %eax
    dec %ecx
    jnz pgt_pd

    mov $pml4_table, %eax
    mov %eax, %cr3
    ret

serial_putc:
    pushl %edx
    pushl %ecx
    pushl %ebx
    movl %eax, %ebx              /* preserve output byte */
    movw $0x3fd, %dx
    movl $200000, %ecx            /* bounded poll counter */
1:
    inb %dx, %al
    testb $0x20, %al              /* THRE */
    jnz 2f
    decl %ecx
    jnz 1b
    popl %ebx
    popl %ecx
    popl %edx                    /* UART unavailable: drop the byte */
    ret
2:
    movl %ebx, %eax
    movw $0x3f8, %dx
    outb %al, %dx
    popl %ebx
    popl %ecx
    popl %edx
    ret

.section .rodata
.align 8
gdt64:
    .quad 0
    .quad 0x00af9a000000ffff   /* 0x08: 64-bit code (L=1, D/B=0) */
    .quad 0x00cf92000000ffff   /* 0x10: data, D/B=1 (32-bit stack in the
                                 *        pre-paging phase) */
gdt64_pointer:
    .word gdt64_pointer - gdt64 - 1
    .long gdt64

.align 8
.section .bss
.align 4096
.global pml4_table
pml4_table:
    .skip 4096
.global pdpt_table
pdpt_table:
    .skip 4096
.global pd_table0
pd_table0:
    .skip 4096
.align 4096
.global pt_low_table
pt_low_table:
    .skip 4096
.global pd_table1
pd_table1:
    .skip 4096
.global pd_table2
pd_table2:
    .skip 4096
.global pd_table3
pd_table3:
    .skip 4096
stack_guard:
    .skip 4096              /* never mapped: a stack overflow faults here */
.global stack_guard
stack_bottom:
    .skip 16384
stack_top:

/*
 * The two words the loader hands over live in .data, not .bss: they are boot
 * state captured from registers, and the .bss clear below runs before they are
 * stored, so they would be wiped.
 */
.section .data
.align 4
.global mb_magic
mb_magic:
    .long 0
.global mb_info
mb_info:
    .long 0

.section .text
.code64
long_mode_entry:
    mov $stack_top, %rsp
    xor %ebp, %ebp
    cld
#if CONFIG_AXYS_CONSOLE_SERIAL
    movb $'H', %al
    call serial_putc
#endif
    movl mb_magic, %edi
    movl mb_info, %esi
    call axys_kmain
axys_halt:
    cli
    hlt
    jmp axys_halt

.section .note.GNU-stack,"",@progbits
