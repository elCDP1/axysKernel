.section .text
.code64
.global axys_idt_flush
axys_idt_flush:
    lidt (%rdi)
    ret

.section .note.GNU-stack,"",@progbits
