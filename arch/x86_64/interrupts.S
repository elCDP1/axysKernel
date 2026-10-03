.section .text
.code64

/*
 * Every vector must arrive at isr_common with the identical stack shape so
 * that a single C struct describes it and `addq $16, %rsp` drops exactly the
 * stub's own two words before iretq:
 *
 *   ... rflags, cs, rip, error, vector, <15 GPRs pushed by isr_common>
 *
 * The CPU's push order is the part that is easy to get backwards. For a fault
 * that carries an error code, the processor saves the saved state FIRST and
 * pushes the error code LAST. Since the stack grows downwards and `push`
 * stores after decrementing, the last thing pushed ends up at the LOWEST
 * address -- so the error code lands directly *below* rip:
 *
 *   error, rip, cs, rflags        <- ascending, as the CPU leaves a #PF
 *
 * Not the other way round. Reading the error from 24(%rsp) grabs rflags, and
 * assuming an ascending frame of `rip, cs, rflags, error` puts the error word
 * above rflags where iretq never looks, which then reads the wrong three words
 * as the return state. Both mistakes are easy to make from memory; the boot
 * self test in kernel/exceptions.c is what actually pins the layout down.
 *
 * So the stubs are trivial: push the vector, and for the vectors that carry no
 * code push a dummy 0 first so the frame is uniform. No error code is ever
 * moved in assembly -- doing that would need a scratch register before
 * isr_common has saved the GPRs, and the only free one (rax) is itself about to
 * be saved, so the handler would report the error code as the interrupted rax.
 */
.macro ISR_NOERR number
.global isr\number
isr\number:
    pushq $0
    pushq $\number
    jmp isr_common
.endm

.macro ISR_ERROR number
.global isr\number
isr\number:
    pushq $\number
    jmp isr_common
.endm

/* #DF is special: its IDT gate uses IST 1. In 64-bit mode an IST stack switch
 * pushes the interrupted SS:RSP onto the new stack in addition to the normal
 * RIP/CS/RFLAGS frame. The generic common path expects the no-switch frame
 * layout, while the epilogue's iretq must preserve the IST frame intact, so
 * #DF gets an otherwise-identical common path without discarding those words. */
.macro ISR_ERROR_IST number
.global isr\number
isr\number:
    pushq $\number
    jmp isr_common_ist
.endm

/*
 * Which vectors push an error code is fixed by the architecture, and getting an
 * entry wrong is not a cosmetic bug: the stub's word count has to match the
 * CPU's exactly, or the frame is off by one and iretq ends up loading a garbage
 * CS out of what is really the saved RFLAGS. The symptom is a #GP raised *at*
 * the iretq, with a selector that looks like nonsense.
 *
 * The authoritative set is: 8 #DF, 10 #TS, 11 #NP, 12 #SS, 13 #GP, 14 #PF,
 * 17 #AC, 21 #CP. Everything else pushes nothing -- notably 0 #DE, 1 #DB,
 * 2 #NMI, 3 #BP, 4 #OF, 5 #BR, 6 #UD, 7 #NM and 20 #VE, which is the part that
 * is easiest to get backwards.
 *
 * 20 #VE is the one that was wrong here. It was listed as error-code on the
 * assumption that the virtualization vectors all behave alike; they do not.
 * Linux declares X86_TRAP_VE = 20 with plain DECLARE_IDTENTRY in
 * asm/idtentry.h, which is has_error_code = 0, so the CPU pushes no code for
 * it. Marking it gave a stub one word too few, which shifts error, rip, cs and
 * rflags up a slot -- harmless until the first #VE, which this kernel cannot
 * currently produce, because #VE is only delivered on a transition into a guest
 * and there is no VMX or SVM guest here. That is exactly how a bug like this
 * survives: no path raises the vector, so no test can observe it.
 *
 * AMD defines the vendor tail as 28 #HV (no error code), 29 #VC (error code),
 * and 30 #SX (error code). Intel reserves these vectors outside configurations
 * that use a corresponding virtualization/security extension. The stubs follow
 * the AMD long-mode contract because those are the cases in which these vectors
 * become architecturally meaningful to this x86-64 kernel.
 */ 
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERROR_IST 8
ISR_NOERR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERROR 17
ISR_NOERR 18
ISR_NOERR 19
/* 20 #VE pushes NO error code. Was ISR_ERROR; see the note above. */
ISR_NOERR 20
ISR_ERROR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERROR 29
ISR_ERROR 30
ISR_NOERR 31

.macro IRQ_NOERR number
.global irq\number
irq\number:
    pushq $0
    pushq $(32 + \number)
    jmp isr_common
.endm

/* Hardware interrupts never push a CPU error code, so every IRQ stub must
 * normalize the frame by pushing a dummy error code. isr_common's epilogue
 * unconditionally removes 16 bytes (error + vector), so a stub that pushes
 * only the vector would desynchronize the frame and corrupt iretq. */
IRQ_NOERR 0
IRQ_NOERR 1
IRQ_NOERR 2
IRQ_NOERR 3
IRQ_NOERR 4
IRQ_NOERR 5
IRQ_NOERR 6
IRQ_NOERR 7
IRQ_NOERR 8
IRQ_NOERR 9
IRQ_NOERR 10
IRQ_NOERR 11
IRQ_NOERR 12
IRQ_NOERR 13
IRQ_NOERR 14
IRQ_NOERR 15

/*
 * Vectors 48..255. Nothing in this kernel is expected to raise these yet,
 * but every IDT slot must point at a present gate: a not-present entry
 * escalates a stray interrupt to #GP, which on a damaged stack is a triple
 * fault. These stubs make that a reportable panic instead.
 *
 * They all push a dummy error code so the frame shape matches the real
 * exception stubs and the C handler needs no special case.
 *
 * Generated by tools/gen_stubs.py.
 */
.macro UNEXPECTED number
.global isr\number
isr\number:
    pushq $0
    pushq $\number
    jmp isr_common
.endm

UNEXPECTED 48
UNEXPECTED 49
UNEXPECTED 50
UNEXPECTED 51
UNEXPECTED 52
UNEXPECTED 53
UNEXPECTED 54
UNEXPECTED 55
UNEXPECTED 56
UNEXPECTED 57
UNEXPECTED 58
UNEXPECTED 59
UNEXPECTED 60
UNEXPECTED 61
UNEXPECTED 62
UNEXPECTED 63
UNEXPECTED 64
UNEXPECTED 65
UNEXPECTED 66
UNEXPECTED 67
UNEXPECTED 68
UNEXPECTED 69
UNEXPECTED 70
UNEXPECTED 71
UNEXPECTED 72
UNEXPECTED 73
UNEXPECTED 74
UNEXPECTED 75
UNEXPECTED 76
UNEXPECTED 77
UNEXPECTED 78
UNEXPECTED 79
UNEXPECTED 80
UNEXPECTED 81
UNEXPECTED 82
UNEXPECTED 83
UNEXPECTED 84
UNEXPECTED 85
UNEXPECTED 86
UNEXPECTED 87
UNEXPECTED 88
UNEXPECTED 89
UNEXPECTED 90
UNEXPECTED 91
UNEXPECTED 92
UNEXPECTED 93
UNEXPECTED 94
UNEXPECTED 95
UNEXPECTED 96
UNEXPECTED 97
UNEXPECTED 98
UNEXPECTED 99
UNEXPECTED 100
UNEXPECTED 101
UNEXPECTED 102
UNEXPECTED 103
UNEXPECTED 104
UNEXPECTED 105
UNEXPECTED 106
UNEXPECTED 107
UNEXPECTED 108
UNEXPECTED 109
UNEXPECTED 110
UNEXPECTED 111
UNEXPECTED 112
UNEXPECTED 113
UNEXPECTED 114
UNEXPECTED 115
UNEXPECTED 116
UNEXPECTED 117
UNEXPECTED 118
UNEXPECTED 119
UNEXPECTED 120
UNEXPECTED 121
UNEXPECTED 122
UNEXPECTED 123
UNEXPECTED 124
UNEXPECTED 125
UNEXPECTED 126
UNEXPECTED 127
UNEXPECTED 128
UNEXPECTED 129
UNEXPECTED 130
UNEXPECTED 131
UNEXPECTED 132
UNEXPECTED 133
UNEXPECTED 134
UNEXPECTED 135
UNEXPECTED 136
UNEXPECTED 137
UNEXPECTED 138
UNEXPECTED 139
UNEXPECTED 140
UNEXPECTED 141
UNEXPECTED 142
UNEXPECTED 143
UNEXPECTED 144
UNEXPECTED 145
UNEXPECTED 146
UNEXPECTED 147
UNEXPECTED 148
UNEXPECTED 149
UNEXPECTED 150
UNEXPECTED 151
UNEXPECTED 152
UNEXPECTED 153
UNEXPECTED 154
UNEXPECTED 155
UNEXPECTED 156
UNEXPECTED 157
UNEXPECTED 158
UNEXPECTED 159
UNEXPECTED 160
UNEXPECTED 161
UNEXPECTED 162
UNEXPECTED 163
UNEXPECTED 164
UNEXPECTED 165
UNEXPECTED 166
UNEXPECTED 167
UNEXPECTED 168
UNEXPECTED 169
UNEXPECTED 170
UNEXPECTED 171
UNEXPECTED 172
UNEXPECTED 173
UNEXPECTED 174
UNEXPECTED 175
UNEXPECTED 176
UNEXPECTED 177
UNEXPECTED 178
UNEXPECTED 179
UNEXPECTED 180
UNEXPECTED 181
UNEXPECTED 182
UNEXPECTED 183
UNEXPECTED 184
UNEXPECTED 185
UNEXPECTED 186
UNEXPECTED 187
UNEXPECTED 188
UNEXPECTED 189
UNEXPECTED 190
UNEXPECTED 191
UNEXPECTED 192
UNEXPECTED 193
UNEXPECTED 194
UNEXPECTED 195
UNEXPECTED 196
UNEXPECTED 197
UNEXPECTED 198
UNEXPECTED 199
UNEXPECTED 200
UNEXPECTED 201
UNEXPECTED 202
UNEXPECTED 203
UNEXPECTED 204
UNEXPECTED 205
UNEXPECTED 206
UNEXPECTED 207
UNEXPECTED 208
UNEXPECTED 209
UNEXPECTED 210
UNEXPECTED 211
UNEXPECTED 212
UNEXPECTED 213
UNEXPECTED 214
UNEXPECTED 215
UNEXPECTED 216
UNEXPECTED 217
UNEXPECTED 218
UNEXPECTED 219
UNEXPECTED 220
UNEXPECTED 221
UNEXPECTED 222
UNEXPECTED 223
UNEXPECTED 224
UNEXPECTED 225
UNEXPECTED 226
UNEXPECTED 227
UNEXPECTED 228
UNEXPECTED 229
UNEXPECTED 230
UNEXPECTED 231
UNEXPECTED 232
UNEXPECTED 233
UNEXPECTED 234
UNEXPECTED 235
UNEXPECTED 236
UNEXPECTED 237
UNEXPECTED 238
UNEXPECTED 239
UNEXPECTED 240
UNEXPECTED 241
UNEXPECTED 242
UNEXPECTED 243
UNEXPECTED 244
UNEXPECTED 245
UNEXPECTED 246
UNEXPECTED 247
UNEXPECTED 248
UNEXPECTED 249
UNEXPECTED 250
UNEXPECTED 251
UNEXPECTED 252
UNEXPECTED 253
UNEXPECTED 254
UNEXPECTED 255

/*
 * One byte per vector: 1 if the CPU pushes an error code for that vector.
 * kernel/interrupts.c compares this against AXYS_IRQ_ERROR_VECTORS so a
 * mismatch between the stub list above and the C mask is caught at boot rather
 * than turning into a one-word-short frame and a #GP raised at iretq.
 *
 * This mirrors the *CPU's* behaviour, not the stubs'. The stubs push a dummy
 * error for every vector the CPU does not, which is the complement of this
 * table; marking the IRQ vectors as 1 here would be answering a different
 * question and would never agree with the mask.
 */
.section .rodata
.align 16
.global axys_error_vector_table
axys_error_vector_table:
    .byte 0, 0, 0, 0, 0, 0, 0, 0     /* 0-7   */
    .byte 1, 0, 1, 1, 1, 1, 1, 0     /* 8-15  */
    /* 16-23. 17 #AC yes, 20 #VE no, 21 #CP yes. 20 was 1 here, which is how
     * it stayed wrong for so long: this table is the mirror the C mask is
     * compared against, so when both were wrong together the check agreed. */
    .byte 0, 1, 0, 0, 0, 1, 0, 0
    .byte 0, 0, 0, 0, 0, 1, 1, 0     /* 24-31: 29 (#VC), 30 (#SX) */
    /* 32-255 are hardware interrupts; the CPU never pushes an error code. */
    .rept 224
    .byte 0
    .endr

/* isr_common is code again -- the table above switched the current section. */
.section .text
.global isr_common
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rbx
    andq $-16, %rsp
    movq %rbx, %rdi
    call axys_isr_handler
    movq %rbx, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    /*
     * Drop the two words the stub pushed (the vector, plus the dummy error for
     * the vectors that carry none). iretq then sees exactly rip/cs/rflags at
     * the top, which is what it needs to resume the interrupted context.
     */
    addq $16, %rsp
    iretq

/* Exact copy of isr_common for an IST-delivered vector such as #DF. The CPU's
 * IST stack frame contains two additional words (old RSP, old SS) after
 * rflags. They are intentionally left untouched so iretq can restore the
 * interrupted stack. The first 20 words are identical to
 * struct axys_interrupt_frame, so the same C handler can consume the prefix. */
.global isr_common_ist
isr_common_ist:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rbx
    andq $-16, %rsp
    movq %rbx, %rdi
    call axys_isr_handler
    movq %rbx, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

.section .rodata
.align 8
/*
 * Stub addresses indexed by vector number: entry N is the handler that
 * pushes N, so axys_idt_init can install the table wholesale.
 *
 * Generated by tools/gen_stub_table.py.
 */
.global axys_isr_stub_table
axys_isr_stub_table:
    .quad isr0
    .quad isr1
    .quad isr2
    .quad isr3
    .quad isr4
    .quad isr5
    .quad isr6
    .quad isr7
    .quad isr8
    .quad isr9
    .quad isr10
    .quad isr11
    .quad isr12
    .quad isr13
    .quad isr14
    .quad isr15
    .quad isr16
    .quad isr17
    .quad isr18
    .quad isr19
    .quad isr20
    .quad isr21
    .quad isr22
    .quad isr23
    .quad isr24
    .quad isr25
    .quad isr26
    .quad isr27
    .quad isr28
    .quad isr29
    .quad isr30
    .quad isr31
    .quad irq0
    .quad irq1
    .quad irq2
    .quad irq3
    .quad irq4
    .quad irq5
    .quad irq6
    .quad irq7
    .quad irq8
    .quad irq9
    .quad irq10
    .quad irq11
    .quad irq12
    .quad irq13
    .quad irq14
    .quad irq15
    .quad isr48
    .quad isr49
    .quad isr50
    .quad isr51
    .quad isr52
    .quad isr53
    .quad isr54
    .quad isr55
    .quad isr56
    .quad isr57
    .quad isr58
    .quad isr59
    .quad isr60
    .quad isr61
    .quad isr62
    .quad isr63
    .quad isr64
    .quad isr65
    .quad isr66
    .quad isr67
    .quad isr68
    .quad isr69
    .quad isr70
    .quad isr71
    .quad isr72
    .quad isr73
    .quad isr74
    .quad isr75
    .quad isr76
    .quad isr77
    .quad isr78
    .quad isr79
    .quad isr80
    .quad isr81
    .quad isr82
    .quad isr83
    .quad isr84
    .quad isr85
    .quad isr86
    .quad isr87
    .quad isr88
    .quad isr89
    .quad isr90
    .quad isr91
    .quad isr92
    .quad isr93
    .quad isr94
    .quad isr95
    .quad isr96
    .quad isr97
    .quad isr98
    .quad isr99
    .quad isr100
    .quad isr101
    .quad isr102
    .quad isr103
    .quad isr104
    .quad isr105
    .quad isr106
    .quad isr107
    .quad isr108
    .quad isr109
    .quad isr110
    .quad isr111
    .quad isr112
    .quad isr113
    .quad isr114
    .quad isr115
    .quad isr116
    .quad isr117
    .quad isr118
    .quad isr119
    .quad isr120
    .quad isr121
    .quad isr122
    .quad isr123
    .quad isr124
    .quad isr125
    .quad isr126
    .quad isr127
    .quad isr128
    .quad isr129
    .quad isr130
    .quad isr131
    .quad isr132
    .quad isr133
    .quad isr134
    .quad isr135
    .quad isr136
    .quad isr137
    .quad isr138
    .quad isr139
    .quad isr140
    .quad isr141
    .quad isr142
    .quad isr143
    .quad isr144
    .quad isr145
    .quad isr146
    .quad isr147
    .quad isr148
    .quad isr149
    .quad isr150
    .quad isr151
    .quad isr152
    .quad isr153
    .quad isr154
    .quad isr155
    .quad isr156
    .quad isr157
    .quad isr158
    .quad isr159
    .quad isr160
    .quad isr161
    .quad isr162
    .quad isr163
    .quad isr164
    .quad isr165
    .quad isr166
    .quad isr167
    .quad isr168
    .quad isr169
    .quad isr170
    .quad isr171
    .quad isr172
    .quad isr173
    .quad isr174
    .quad isr175
    .quad isr176
    .quad isr177
    .quad isr178
    .quad isr179
    .quad isr180
    .quad isr181
    .quad isr182
    .quad isr183
    .quad isr184
    .quad isr185
    .quad isr186
    .quad isr187
    .quad isr188
    .quad isr189
    .quad isr190
    .quad isr191
    .quad isr192
    .quad isr193
    .quad isr194
    .quad isr195
    .quad isr196
    .quad isr197
    .quad isr198
    .quad isr199
    .quad isr200
    .quad isr201
    .quad isr202
    .quad isr203
    .quad isr204
    .quad isr205
    .quad isr206
    .quad isr207
    .quad isr208
    .quad isr209
    .quad isr210
    .quad isr211
    .quad isr212
    .quad isr213
    .quad isr214
    .quad isr215
    .quad isr216
    .quad isr217
    .quad isr218
    .quad isr219
    .quad isr220
    .quad isr221
    .quad isr222
    .quad isr223
    .quad isr224
    .quad isr225
    .quad isr226
    .quad isr227
    .quad isr228
    .quad isr229
    .quad isr230
    .quad isr231
    .quad isr232
    .quad isr233
    .quad isr234
    .quad isr235
    .quad isr236
    .quad isr237
    .quad isr238
    .quad isr239
    .quad isr240
    .quad isr241
    .quad isr242
    .quad isr243
    .quad isr244
    .quad isr245
    .quad isr246
    .quad isr247
    .quad isr248
    .quad isr249
    .quad isr250
    .quad isr251
    .quad isr252
    .quad isr253
    .quad isr254
    .quad isr255

.section .note.GNU-stack,"",@progbits
