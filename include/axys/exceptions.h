#ifndef AXYS_EXCEPTIONS_H
#define AXYS_EXCEPTIONS_H

#include "axys/interrupts.h"
#include "axys/types.h"

/*
 * Snapshot of a CPU exception, decoded into something printable. Keeping this
 * separate from struct axys_interrupt_frame means the raw stack layout stays
 * a private contract between the assembly and the decoder.
 */
struct axys_exception_info {
    axys_uint32_t vector;
    axys_uint64_t error_code;

    axys_uint64_t error_selector; /* #GP/#NP/#SS: selector index from the code */
    int has_error_selector;

    /* #PF only: CR2, the linear address that failed to translate. This field
     * is never populated for selector-style exceptions -- their error code is
     * segment-selector information, not a memory address. */
    axys_uint64_t fault_address;
    int has_fault_address;

    /*
     * Decoded error-code bits. These are -1 where the field has no meaning for
     * the vector in question, which is most of them for most vectors: the error
     * code format is per-vector, and forcing one uniform layout across all of
     * them produces confidently wrong output. A decoder that says "not
     * applicable" is worth more than one that guesses.
     *
     *   #PF: present/write/user/reserved are real. `instruction` is populated
     *        when NX is supported/enabled; then error-code bit 4 is the CPU's
     *        instruction-fetch flag. There is no page-table index in this code.
     *   #GP/#NP/#SS: the error code is a selector-style code. `idt` reflects
     *        its IDT-table indicator bit; present/write/user are otherwise -1.
     *   #DF: the processor supplies an error code of zero.
     *   #DE/#DB/#BR/#OF/#NM/#MF/#AC/#CP/#VE/#VC/#SEV: -1 unless the vector's
     *        manual entry says otherwise.
     */
    int present;      /* 0 = the segment/page was not present      */
    int write;        /* 1 = the access was a write                */
    int user;         /* 1 = the access was from CPL 3             */
    int reserved;     /* 1 = reserved bit was set                 */
    int instruction;  /* 1 = the fault was on instruction fetch   */
    int idt;          /* 1 = the gate itself was not present      */

    axys_uint64_t rip;
    axys_uint64_t cs;
    axys_uint64_t rflags;
    axys_uint64_t rsp;
    axys_uint64_t rbp;
};

/* Fill *info from a raw interrupt frame. Does not touch hardware state. */
void axys_exception_decode(const struct axys_interrupt_frame *frame,
                           struct axys_exception_info *info);

/*
 * Hardware frame shapes.
 *
 * The C-visible prefix (15 GPRs + vector + error + RIP + CS + RFLAGS) is the
 * kernel's own construction; what the CPU pushes after RFLAGS depends on how
 * delivery happened:
 *
 *   - same-CPL entry without IST: nothing more is pushed. There is no saved
 *     RSP/SS word at all, so info->rsp is an ESTIMATE (the current stack
 *     pointer inside the handler) rather than a recovered value.
 *   - privilege transition (CS.RPL == 3): the CPU pushes SS then RSP below
 *     RFLAGS, i.e. immediately after the software prefix. Those words are
 *     real and readable.
 *   - IST delivery: the CPU switches to the TSS IST stack first, then pushes
 *     the same optional SS:RSP pair -- always present for IST entries in
 *     64-bit mode, because the switch itself is a stack change.
 *
 * These predicates let callers tell "the exact interrupted RSP" apart from
 * "the best guess we can make", which matters when deciding whether a fault
 * came from user code.
 */
int axys_exception_has_saved_rsp(const struct axys_interrupt_frame *frame);
axys_uint64_t axys_exception_saved_rsp(const struct axys_interrupt_frame *frame);
axys_uint16_t axys_exception_saved_ss(const struct axys_interrupt_frame *frame);

/* Print a full diagnostic: name, vector, decoded error code, fault address and
 * every general-purpose register. Safe to call from the exception path. */
void axys_exception_report(const struct axys_interrupt_frame *frame);

/* Enable/disable the deliberate-fault self test (see below). */
void axys_fault_test_enable(int enabled);
int axys_fault_test_enabled(void);

/*
 * Deliberate fault injection, used by the boot self test to prove the
 * exception path actually decodes a vector correctly.
 *
 * These intentionally raise a CPU exception. The handler recognises the
 * test, records the vector it saw, and returns without panicking, so the test
 * can assert on the result. axys_fault_test_enable must gate them: an
 * attacker who could otherwise reach ud2 must not get a "harmless" path.
 *
 * Returns the vector the CPU actually raised, or 0xffffffff if the test was
 * not armed or the selector was unknown.
 */
axys_uint32_t axys_fault_test_last_vector(void);

/*
 * Where the frame for the last absorbed fault was built, as a stack address.
 *
 * Only meaningful straight after axys_fault_test_run returns a real vector. It
 * exists to answer the question axys_fault_test_last_vector cannot: did the CPU
 * actually switch to the dedicated stack, or did the handler just happen to
 * work on whatever stack was current? Those look identical from the vector
 * alone, and the second is the failure mode that turns a recoverable #DF into a
 * triple fault.
 */
axys_uintptr_t axys_fault_test_last_frame_sp(void);

axys_uint32_t axys_fault_test_run(axys_uint32_t which);

/* Entry point for every CPU exception, called by axys_isr_handler. */
void axys_exception_entry(struct axys_interrupt_frame *frame);

/*
 * Boot self test: injects one fault of each interesting shape and reports
 * whether the handler decoded the expected vector. Run once during startup to
 * prove the IDT, stubs, frame layout and dispatcher actually agree.
 */
void axys_exception_selftest(void);

/* Which fault to inject. */
#define AXYS_FAULT_UD2 0   /* #UD invalid opcode (vector 6)   */
#define AXYS_FAULT_BP  1   /* #BP breakpoint (vector 3, int3) */
#define AXYS_FAULT_DE  2   /* #DE divide error (vector 0)     */
#define AXYS_FAULT_PF  3   /* #PF page fault (vector 14)      */
#define AXYS_FAULT_DF  4   /* #DF double fault (vector 8)     */
#define AXYS_FAULT_IST 5   /* delivered on IST 1 (vector 22)  */

/*
 * Vector 22 is reserved: the architecture assigns it no exception, so nothing
 * can raise it by accident. The IST self test borrows it as a scratch gate with
 * IST 1 installed, which lets the boot test confirm the CPU honours the gate's
 * IST byte -- the same mechanism, and the same decoding, that #DF recovery
 * depends on. See axys_exception_selftest.
 */
#define AXYS_FAULT_TEST_IST_VECTOR 22

/* Raw trap generators from arch/x86_64/fault.S. Each traps by design. */
void axys_fault_ud2(void);
void axys_fault_breakpoint(void);
void axys_fault_divide(void);
void axys_fault_page(void);
/* Raises a genuine #DF and recovers on IST 1. Only safe to call once IST 1 is
 * known good: if that path regresses, the CPU keeps escalating until it resets
 * and takes the whole machine with it. That is the point of the test.
 *
 * Not part of the default boot sequence -- see the note in
 * axys_exception_selftest -- and callable only where a reset is acceptable. */
void axys_fault_double(void);
/* Raises a software interrupt through the scratch IST 1 gate. Returns normally
 * only if the CPU honoured the gate's IST field. */
void axys_fault_ist(void);

/*
 * Where to resume after absorbing an injected fault. The generators write this
 * from assembly just before they trap, so it always points just past the
 * trapping instruction. It is defined in kernel/exceptions.c and referenced
 * directly by fault.S.
 */
extern axys_uintptr_t axys_fault_resume_rip;

#endif
