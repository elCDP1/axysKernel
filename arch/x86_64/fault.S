.section .text
.code64

/*
 * Deliberate exception generators for the boot self test.
 *
 * These exist because the faulting instructions (ud2, int3, a dividing by
 * zero, an unmapped load) cannot be written in C portably -- the compiler is
 * free to constant-fold or optimise all of them away, and -O2 does.
 *
 * Each generator first publishes the address of the instruction *after* its
 * trapping instruction into axys_fault_resume_rip, then traps. The exception
 * handler reads that global and redirects the saved RIP there, so the trap is
 * absorbed and the generator simply returns.
 *
 * The resume label deliberately lives here, in the assembler, rather than
 * being a C computed goto (&&label). GCC is entitled to place a label address
 * wherever it likes -- folding it onto the call site, for instance -- and when
 * it does, the "resume" address lands back on the call and re-enters the trap
 * forever. An assembler label adjacent to the instruction it follows cannot
 * drift like that.
 */

.global axys_fault_ud2
axys_fault_ud2:
    leaq .Lud2_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)
    ud2
.Lud2_after:
    ret

.global axys_fault_breakpoint
axys_fault_breakpoint:
    leaq .Lbp_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)
    int3
.Lbp_after:
    ret

.global axys_fault_divide
axys_fault_divide:
    leaq .Lde_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)
    /*
     * xor %edx,%edx makes both the dividend and the divisor zero, which is
     * #DE regardless of the dividend. rax is set non-zero so nothing treats
     * 0/0 as a special case.
     */
    movq $1, %rax
    xorl %edx, %edx
    divl %edx
.Lde_after:
    ret

.global axys_fault_page
axys_fault_page:
    leaq .Lpf_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)
    /* The VMM identity-maps usable RAM up to 512 GiB, but never maps the next
     * PML4 slot. This canonical address is therefore a stable unmapped target
     * even on machines with RAM above the old 4 GiB boundary. */
    movq $0x0000008000000000, %rax
    movq (%rax), %rax
.Lpf_after:
    ret

/*
 * Double fault.
 *
 * A #DF is by definition a *second* fault, so it cannot be raised directly --
 * there is no instruction that generates one. The only way to produce one is to
 * fault in a situation where the CPU cannot also push a valid frame onto the
 * current stack, which is normally the stack being unusable. That is exactly
 * what the CPU does here on its own: it detects a non-canonical stack switch
 * while delivering the #UD, escalates to #DF, and the handler runs on IST 1.
 *
 * The critical detail is that IST 1 has to work, or this faults again and again
 * until the machine resets -- which is what a deliberate #DF looked like before
 * the IDT's IST byte was shifted into bits 6-4. That byte was written as the raw
 * index, so the gate decoded as IST 0 with a reserved gate type of 1; every
 * delivery faulted instead of switching stacks.
 *
 * The frame the #DF handler sees is NOT this one. The CPU's own frame sits on
 * the IST stack, and the saved RIP points at the `ud2`, whose resume label is
 * .Ldf_after, so publishing that as the resume point lets the handler unwind
 * straight back into C.
 */
.global axys_fault_double
axys_fault_double:
    /*
     * Save the callee-saved registers the ABI requires before changing RSP.
     * The kernel's own idle path, and any call chain above this, may still need
     * them, and the .Ldf_after epilogue restores them. 16 bytes, so RSP stays
     * 16-byte aligned per the SysV convention -- which also keeps the
     * frame-pointer arithmetic in isr_common honest.
     */
    pushq %rbx
    pushq %rbp
    movq %rsp, %rbp

    /*
     * Park the resume address where the C handler can find it, but do it
     * *before* the stack is trashed: writing to the IST stack from here is
     * unreliable precisely because the IST path is what is under test.
     */
    leaq .Ldf_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)

    /* 0x0000800000000000 is deliberately non-canonical in 48-bit mode, so
     * the processor cannot push the #UD frame there and must escalate to #DF. */
    movq $0x0000800000000000, %rsp
    ud2

.Ldf_after:
    /*
     * Reached either by the #DF handler redirecting RIP here, or (if the
     * escalation path ever stops working) never. RSP is whatever the handler
     * left behind, so it is reloaded from the saved frame before the pops.
     */
    movq %rbp, %rsp
    popq %rbp
    popq %rbx
    ret

/*
 * IST delivery probe.
 *
 * Raises a software interrupt through a scratch gate on IST 1. Two properties
 * make this the only reliable check that the IST wiring works:
 *
 *   1. The IST field is honoured for software interrupts, exactly as for
 *      exceptions -- it is a property of the gate, not of how the vector was
 *      raised. So this exercises the same CPU path #DF recovery depends on.
 *   2. A software `int` pushes no error code, whatever the gate says. That is
 *      the one asymmetry in the error-code layout, and it is why this must use
 *      a no-error vector: `int $8` against the #DF gate would push no code while
 *      the stub assumes one, desynchronising the frame instead of testing it.
 *
 * The C side then checks the address the frame was built at against the IST
 * stack bounds, which is the assertion that actually distinguishes "honoured
 * IST 1" from "ran on the current stack and happened to work".
 */
.global axys_fault_ist
axys_fault_ist:
    /*
     * The handler returns via iretq onto the IST stack, so RSP has to be
     * restored from a callee-saved register before the ret, or the return
     * address is popped from the dedicated stack and execution jumps into
     * whatever is stored there.
     */
    pushq %rbp
    movq %rsp, %rbp

    leaq .List_after(%rip), %rax
    movq %rax, axys_fault_resume_rip(%rip)

    /* int $22, encoded long form so no imm8 truncation ambiguity. */
    .byte 0xcd
    .byte 0x16

.List_after:
    movq %rbp, %rsp
    popq %rbp
    ret


.section .note.GNU-stack,"",@progbits
