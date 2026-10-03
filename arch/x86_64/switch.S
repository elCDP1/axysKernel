/*
 * Kernel context switch.
 *
 *   void axys_context_switch(uint64_t *save_rsp, uint64_t new_rsp);
 *
 * Only the System V callee-saved registers need saving: the caller is an
 * ordinary C function call, so everything else is already dead. The new stack
 * is popped in the mirror order and `ret` resumes wherever that task last
 * called axys_context_switch -- or, for a brand-new task, at
 * axys_task_trampoline with r12 = entry and r13 = argument.
 */
.text
.global axys_context_switch
axys_context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.global axys_task_trampoline
axys_task_trampoline:
    sti                     /* a new task starts with interrupts enabled */
    movq %r13, %rdi
    callq *%r12
    xorl %edi, %edi
    callq axys_task_exit    /* entry returned: exit(0); never comes back */
    ud2

.section .note.GNU-stack,"",@progbits
