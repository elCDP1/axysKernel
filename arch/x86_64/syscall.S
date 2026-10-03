/*
 * SYSCALL entry and the ring-3 launch stub.
 *
 * On SYSCALL the CPU has already put the user RIP in RCX and RFLAGS in R11,
 * loaded CS/SS from STAR, masked RFLAGS with SFMASK (IF, DF, TF, AC... off) and
 * left RSP untouched -- still the *user* stack. The first job is therefore to
 * switch to this task's kernel stack (kept in axys_syscall_kernel_rsp by the
 * scheduler) without trusting anything about the user RSP, then build a
 * struct axys_syscall_frame (see kernel/syscall.c) and call the C dispatcher.
 * Interrupts stay off only until the user RSP is safely on the kernel stack.
 */
.text
.global axys_syscall_entry
axys_syscall_entry:
    movq %rsp, axys_syscall_scratch(%rip)
    movq axys_syscall_kernel_rsp(%rip), %rsp
    pushq axys_syscall_scratch(%rip)   /* user rsp */
    pushq %rcx                         /* user rip */
    pushq %r11                         /* user rflags */
    pushq %rax                         /* syscall number */
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    sti
    cld
    call axys_syscall_dispatch
    cli
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax                          /* result written back by the dispatcher */
    popq %r11
    popq %rcx
    popq %rsp                          /* back on the user stack */
    sysretq

/* void axys_enter_user(rip=rdi, rsp=rsi, arg0=rdx, arg1=rcx): never returns. */
.global axys_enter_user
axys_enter_user:
    cli
    pushq $0x1b            /* SS  = user data | RPL3 */
    pushq %rsi             /* RSP */
    pushq $0x202           /* RFLAGS: IF=1, IOPL=0 */
    pushq $0x23            /* CS  = user code | RPL3 */
    pushq %rdi             /* RIP */
    movq %rdx, %rdi
    movq %rcx, %rsi
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq

.section .bss
.align 8
.global axys_syscall_scratch
axys_syscall_scratch:
    .skip 8

.section .note.GNU-stack,"",@progbits
