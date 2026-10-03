.section .text
.code64
.global axys_gdt_flush
axys_gdt_flush:
    lgdt (%rdi)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    ret

.global axys_tss_load
axys_tss_load:
    mov $0x28, %ax
    ltr %ax
    ret

.section .note.GNU-stack,"",@progbits
