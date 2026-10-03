.text
.globl _start
_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    call main               /* rdi = args pointer, rsi = args length (from the kernel) */
    movl %eax, %edi
    xorl %eax, %eax         /* SYS_EXIT */
    syscall
    ud2
.section .note.GNU-stack,"",@progbits
