#ifndef AXYS_INTERRUPTS_H
#define AXYS_INTERRUPTS_H

#include "axys/types.h"

#include "axys/idt.h" /* for AXYS_IDT_* vector constants */

/*
 * Register snapshot taken on every interrupt/exception, describing the stack
 * that arch/x86_64/interrupts.S builds. isr_common calls the handler with
 * %rdi = %rsp, so the first field below must be r15: the GPRs are pushed in
 * order rax -> r15 and the stack grows down, so r15 ends up lowest.
 *
 * The frame is, ascending from rsp:
 *
 *   r15, r14, .. r8, rbp, rdi, rsi, rdx, rcx, rbx, rax,   15 GPRs
 *   vector, error, rip, cs, rflags                          5 words
 *
 * The error word is always present: the stubs push a dummy 0 for the vectors
 * that carry no code, so one struct covers all 256 ordinary entries. The #DF
 * gate is the deliberate exception: it uses IST 1, so long mode appends the
 * interrupted SS:RSP after rflags. The IST-specific assembly path leaves those
 * two words in place for iretq; the C handler only consumes the common 20-word
 * prefix.
 *
 * Getting this order wrong still compiles, still links and still boots -- it
 * just makes every handler read the wrong slot, so a dispatcher would compare
 * against a saved GPR instead of the real vector and iretq would resume
 * garbage. tests/test_interrupt_frame.c guards the layout, and
 * axys_exception_selftest() re-checks it against the real CPU at boot.
 */
struct axys_interrupt_frame {
    axys_uint64_t r15;
    axys_uint64_t r14;
    axys_uint64_t r13;
    axys_uint64_t r12;
    axys_uint64_t r11;
    axys_uint64_t r10;
    axys_uint64_t r9;
    axys_uint64_t r8;
    axys_uint64_t rbp;
    axys_uint64_t rdi;
    axys_uint64_t rsi;
    axys_uint64_t rdx;
    axys_uint64_t rcx;
    axys_uint64_t rbx;
    axys_uint64_t rax;
    axys_uint64_t vector;
    axys_uint64_t error;
    axys_uint64_t rip;
    axys_uint64_t cs;
    axys_uint64_t rflags;
};

/* 15 GPRs + vector + error + rip + cs + rflags. */
#define AXYS_INTERRUPT_FRAME_WORDS 20
#define AXYS_INTERRUPT_FRAME_BYTES (AXYS_INTERRUPT_FRAME_WORDS * 8)

/*
 * Vectors whose CPU entry pushes a real error code; for every other vector
 * frame->error is the dummy 0 the stub supplied. This must stay in sync with
 * the ISR_ERROR/ISR_NOERR list in arch/x86_64/interrupts.S -- the two are the
 * same fact expressed twice, and a mismatch silently shifts the frame by one
 * word (Intel SDM Table 13-1).
 *
 * Authoritative, for every x86-64 part: 8 #DF, 10 #TS, 11 #NP, 12 #SS, 13 #GP,
 * 14 #PF, 17 #AC, 21 #CP. Linux declares 21 with DECLARE_IDTENTRY_ERRORCODE,
 * i.e. has_error_code = 1.
 *
 * NOT 20 #VE. It was marked here on the belief that all three virtualization
 * vectors behave alike, and Linux disagrees: trapnr.h defines X86_TRAP_VE = 20
 * and idtentry.h declares it with plain DECLARE_IDTENTRY, has_error_code = 0. It
 * pushes no code. Marking it was a latent frame desync of one word -- the stub
 * would skip the dummy push the CPU never made, so error, rip, cs and rflags
 * would all be read one slot too high and iretq would return to garbage. It
 * never showed up because nothing in this kernel can raise #VE: there is no
 * VMX or SVM guest, and #VE is only delivered on a transition to a guest.
 *
 * Vendor tail: AMD defines 28 as #HV (Hypervisor Injection, no error code),
 * 29 as #VC (VMM Communication, pushes the VMEXIT code), and 30 as #SX
 * (Security Exception, pushes an error code). These are not part of this
 * kernel's SEV/TDX support yet, but the entry-frame layout must still match the
 * architecture or a future secure-guest boot will corrupt the return frame.
 */
#define AXYS_IRQ_ERROR_VECTORS                                                  \
    ((1ull << 8) | (1ull << 10) | (1ull << 11) | (1ull << 12) |                \
     (1ull << 13) | (1ull << 14) | (1ull << 17) | (1ull << 21) |                \
     (1ull << 29) | (1ull << 30))

/* True if *frame's vector carries a genuine CPU error code. */
static AXYS_ALWAYS_INLINE int
axys_interrupt_has_error(const struct axys_interrupt_frame *frame)
{
    return frame->vector < 64 &&
           (AXYS_IRQ_ERROR_VECTORS & (1ull << (axys_uint64_t)frame->vector)) != 0;
}

/*
 * The IDT descriptor knows, for every vector, whether its stub normalises an
 * error code. Defined in assembly (see the axys_error_vector_table symbol in
 * arch/x86_64/interrupts.S) so the C mask and the actual stub table are checked
 * against each other rather than merely kept in sync by hand.
 */
extern const axys_uint8_t axys_error_vector_table[256];

/* Returns the number of vectors where the stub table and AXYS_IRQ_ERROR_VECTORS
 * disagree. Must be 0. A non-zero result means some frames are one word short
 * and iretq will fault. */
unsigned axys_interrupt_layout_mismatches(void);

/*
 * Address of the assembly stub for each of the 256 vectors, built by
 * arch/x86_64/interrupts.S. Indexed by vector number.
 *
 * Declared here rather than privately in kernel/idt.c because the boot self
 * test needs it too: to install an IST gate it has to reuse the stub for that
 * vector, and reaching into the stub table is what keeps the handler identical
 * to the one the normal IDT setup would have chosen.
 */
extern axys_uintptr_t axys_isr_stub_table[AXYS_IDT_ENTRIES];

/* If this ever trips, the struct above no longer describes the stack that
 * isr_common builds, and every field access in the exception path is wrong. */
AXYS_STATIC_ASSERT(sizeof(struct axys_interrupt_frame) == AXYS_INTERRUPT_FRAME_BYTES,
                   interrupt_frame_size);

void axys_isr_handler(struct axys_interrupt_frame *frame);

/*
 * 8259A pair. Both are remapped so the CPU exception vectors 0..31 stay clear,
 * and both start fully masked: an unmasked line during early boot would
 * deliver a vector whose handler is not installed yet.
 */
void axys_pic_remap(void);
void axys_pic_unmask(axys_uint8_t irq);
void axys_pic_mask(axys_uint8_t irq);
void axys_pic_mask_all(void);
void axys_pic_unmask_all_but_cascade(void);

/* A per-IRQ handler. Receives the saved frame; must not clobber the stack. */
typedef void (*axys_irq_handler_t)(struct axys_interrupt_frame *frame);

/* Install a handler for a hardware IRQ (0..15). Pass AXYS_NULL to detach. */
void axys_irq_register(axys_uint8_t irq, axys_irq_handler_t handler);

/* Per-IRQ counters, so a driver (or the boot log) can prove its line fires. */
axys_uint64_t axys_irq_count(axys_uint8_t irq);
axys_uint64_t axys_irq_total(void);
void axys_irq_reset_counters(void);

#endif
