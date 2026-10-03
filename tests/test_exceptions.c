#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "axys/exceptions.h"
#include "axys/interrupts.h"
#include "axys/types.h"

/*
 * Host-side check of axys_exception_decode, which runs before any hardware is
 * touched so every branch here is reachable from a test.
 *
 * The only part of this worth a test is the one the compiler cannot check: the
 * decoder reconstructs the interrupted RSP from the frame pointer, and it got
 * the sign wrong. `frame` points at the LOWEST address of the frame, so the
 * interrupted stack pointer is frame + 160, not frame - 160. Subtracting lands
 * inside the interrupted code's own frame, which is normally still mapped, so
 * the bug printed a confident wrong value in every exception report rather than
 * crashing -- which is the worst kind.
 */

#define FRAME_BYTES AXYS_INTERRUPT_FRAME_BYTES

static struct axys_interrupt_frame *make_frame(axys_uint32_t vector,
                                               axys_uint64_t error)
{
    /* The frame has to sit entirely inside the backing store: 160 bytes at
     * offset 64 needs 224, so 48 words (384 bytes) leaves room to spare. */
    static axys_uint64_t backing[48];
    struct axys_interrupt_frame *frame =
        (struct axys_interrupt_frame *)&backing[8];

    memset(frame, 0, FRAME_BYTES);
    frame->vector = vector;
    frame->error = error;
    frame->rip = 0x1000u;
    frame->cs = 0x08u;
    frame->rflags = 0x202u;
    frame->rbp = 0x2000u;
    return frame;
}

/* The interrupted RSP is everything above the frame, never below it. */
static void test_rsp_is_above_the_frame(void)
{
    struct axys_exception_info info;
    struct axys_interrupt_frame *frame = make_frame(6, 0);
    const axys_uint64_t expected =
        (axys_uint64_t)(axys_uintptr_t)frame + FRAME_BYTES;

    axys_exception_decode(frame, &info);
    assert(info.rsp == expected);
    assert(info.rsp > (axys_uint64_t)(axys_uintptr_t)frame);
}

/* Same answer regardless of the vector: our software-visible prefix is fixed.
 * #DF also uses IST, but its saved old RSP begins at the same offset after the
 * prefix; the additional old SS word follows it. */
static void test_rsp_independent_of_vector(void)
{
    static const axys_uint32_t vectors[] = { 0, 1, 3, 6, 8, 13, 14, 19, 21 };
    axys_size_t index;

    for (index = 0; index < sizeof(vectors) / sizeof(vectors[0]); ++index) {
        struct axys_exception_info info;
        struct axys_interrupt_frame *frame = make_frame(vectors[index], 0);

        axys_exception_decode(frame, &info);
        assert(info.rsp ==
               (axys_uint64_t)(axys_uintptr_t)frame + FRAME_BYTES);
        assert(info.vector == vectors[index]);
    }
}

/* #PF error code decoding. Bits 0-3 are P/W/U/RS. Bit 4 is the instruction
 * fetch flag when NX is supported/enabled; this host stub deliberately returns
 * false, so the decoder must leave that field not-applicable. */
static void test_page_fault_decode(void)
{
    struct axys_exception_info info;
    /* present=1 write=1 user=0 reserved=1, plus an arbitrary bit 4. */
    struct axys_interrupt_frame *frame = make_frame(14, (1u | 2u | 8u | 1u << 4));

    axys_exception_decode(frame, &info);
    assert(info.vector == 14);
    assert(info.error_code == (1u | 2u | 8u | 1u << 4));
    assert(info.present == 1);
    assert(info.write == 1);
    assert(info.user == 0);
    assert(info.reserved == 1);
    assert(axys_interrupt_has_error(frame));
}

/*
 * #GP/#NP/#SS use selector-style error codes, not a present/write/user triple.
 * Bit 1 is the IDT indicator and bit 2 is the TI indicator; the low bits are not
 * the page-fault P/W/U fields. Those fields must stay -1 and the fault address
 * is the selector index portion.
 */
static void test_selector_codes_are_not_pwf_bits(void)
{
    static const axys_uint32_t vectors[] = { 11, 12, 13 };
    axys_size_t index;

    for (index = 0; index < 3; ++index) {
        struct axys_exception_info info;
        /* 0x0043: selector index 8 with the IDT error-code indicator set. */
        struct axys_interrupt_frame *frame = make_frame(vectors[index], 0x0043u);

        axys_exception_decode(frame, &info);
        assert(info.vector == vectors[index]);
        assert(info.present == -1);
        assert(info.write == -1);
        assert(info.user == -1);
        assert(info.idt == 1); /* error-code bit 1 is the external/IDT indicator */
        /* Selector-style errors expose selector information, never a memory
         * fault address: fault_address is reserved for CR2 on #PF. */
        assert(info.has_error_selector == 1);
        assert(info.error_selector == 0x0040u);
        assert(info.has_fault_address == 0);
    }
}

/* #DF always supplies an error code of zero; it has no selector/page-style
 * subfields to decode. */
static void test_double_fault_decode(void)
{
    struct axys_exception_info info;
    struct axys_interrupt_frame *frame = make_frame(8, 0);

    axys_exception_decode(frame, &info);
    assert(info.vector == 8);
    assert(info.error_code == 0);
    assert(info.present == -1);
    assert(info.write == -1);
    assert(info.user == -1);
    assert(info.has_fault_address == 0);
}

/*
 * A vector that pushes no code must not have its dummy zero decoded: the fields
 * stay -1; there is no page-table index field in a page-fault error code.
 */
static void test_codeless_vector_is_not_decoded(void)
{
    struct axys_exception_info info;
    struct axys_interrupt_frame *frame = make_frame(6, 0);

    assert(!axys_interrupt_has_error(frame));
    axys_exception_decode(frame, &info);
    assert(info.present == -1);
    assert(info.write == -1);
    assert(info.user == -1);
    assert(info.reserved == -1);
    assert(info.instruction == -1);
    assert(info.idt == -1);
    assert(info.has_fault_address == 0);
}

/* AMD's vendor-defined tail: #HV (28) has no error code, #VC (29) carries
 * the VMEXIT code, and #SX (30) carries its security exception code. */
static void test_vendor_error_vectors(void)
{
    assert(!axys_interrupt_has_error(make_frame(28, 0)));
    assert(axys_interrupt_has_error(make_frame(29, 0x1u)));
    assert(axys_interrupt_has_error(make_frame(30, 0x1u)));

    /* And the neighbours really do push nothing. */
    assert(!axys_interrupt_has_error(make_frame(31, 0)));
    assert(!axys_interrupt_has_error(make_frame(27, 0)));
}

/* Every name lookup in range must be non-empty, and out of range must not read
 * past the table.
 *
 * The completeness check is the point. kernel/idt.c used a positional list of 31
 * strings for a 32-element table, so names[31] was NULL and every label from
 * index 22 up was shifted by one. Nothing caught it: the exception reporter
 * passes the result to %s, and printf substitutes "(null)" for a NULL pointer,
 * so the mislabel only ever showed up as a wrong -- never a missing -- name. A
 * caller that compared or measured it would have faulted. */
static void test_exception_names(void)
{
    axys_uint32_t vector;

    for (vector = 0; vector < AXYS_IDT_EXCEPTION_COUNT; ++vector) {
        const char *name = axys_exception_name(vector);

        assert(name != NULL);
        assert(name[0] != '\0');
        assert(strcmp(name, "unknown") != 0);
        /* Names must be unique, which also pins them to the right index. */
        assert(strcmp(name, axys_exception_name(AXYS_IDT_EXCEPTION_COUNT)) != 0);
    }

    /* Out of range must not index the table at all. */
    assert(strcmp(axys_exception_name(AXYS_IDT_EXCEPTION_COUNT), "unknown") == 0);
    assert(strcmp(axys_exception_name(32), "unknown") == 0);
    assert(strcmp(axys_exception_name(255), "unknown") == 0);
    assert(strcmp(axys_exception_name(0xffffffffu), "unknown") == 0);

    /* Spot-check the ones the kernel actually reacts to. */
    assert(strcmp(axys_exception_name(6), "invalid opcode") == 0);
    assert(strcmp(axys_exception_name(8), "double fault") == 0);
    assert(strcmp(axys_exception_name(13), "general protection fault") == 0);
    assert(strcmp(axys_exception_name(14), "page fault") == 0);

    /* Pin the AMD-specific tail to its architectural vector numbers. */
    assert(strcmp(axys_exception_name(27), "reserved (27)") == 0);
    assert(strstr(axys_exception_name(28), "hypervisor injection") != NULL);
    assert(strstr(axys_exception_name(29), "VMM communication") != NULL);
    assert(strstr(axys_exception_name(30), "security exception") != NULL);
    assert(strcmp(axys_exception_name(31), "reserved (31)") == 0);
}

/*
 * The error-code mask, checked against an independently written table.
 *
 * The kernel already cross-checks AXYS_IRQ_ERROR_VECTORS against the table the
 * assembly emits, which catches the two drifting apart. It cannot catch both
 * being wrong together, so the expected answer is spelled out here as literals.
 *
 * The values are split by how they are known, because that distinction is the
 * whole point. `authoritative` is what the architecture and Linux's own entry
 * declarations say, and it is a genuine cross-check against an outside source.
 * `vendor` is the AMD SEV tail, which Linux does not define at all and which
 * this kernel cannot currently receive: it is a documented decision, not a
 * verified fact, and writing it as a plain literal would quietly promote it to
 * one.
 *
 * 20 #VE is the regression this table exists to catch. It was 1, on the
 * assumption that the virtualization vectors all carry a code. They do not:
 * Linux declares X86_TRAP_VE with plain DECLARE_IDTENTRY, has_error_code = 0.
 */
static void test_error_mask_matches_architecture(void)
{
    /* What the architecture fixes, cross-checked against Linux master:
     * asm/idtentry.h uses DECLARE_IDTENTRY_ERRORCODE for exactly these, and
     * DECLARE_IDTENTRY (has_error_code = 0) for the others.
     *
     * Designated by vector, never positional. A positional list with a comment
     * beside each row looks self-documenting and silently shifts the moment a
     * row is removed -- which is what happened while writing this very table,
     * when dropping the two vendor entries put 1 at index 28. The error-code
     * mask in kernel/idt.c had the same defect and shipped it. */
    static const axys_uint32_t authoritative[32] = {
        [0] = 0,   /* #DE  */
        [1] = 0,   /* #DB  */
        [2] = 0,   /* #NMI */
        [3] = 0,   /* #BP  */
        [4] = 0,   /* #OF  */
        [5] = 0,   /* #BR  */
        [6] = 0,   /* #UD  */
        [7] = 0,   /* #NM  */
        [8] = 1,   /* #DF  */
        [9] = 0,   /* #MF, deprecated */
        [10] = 1,  /* #TS  */
        [11] = 1,  /* #NP  */
        [12] = 1,  /* #SS  */
        [13] = 1,  /* #GP  */
        [14] = 1,  /* #PF  */
        [15] = 0,  /* reserved */
        [16] = 0,  /* #MF  */
        [17] = 1,  /* #AC  */
        [18] = 0,  /* #MC  */
        [19] = 0,  /* #XM  */
        [20] = 0,  /* #VE: no code, Linux uses plain DECLARE_IDTENTRY */
        [21] = 1,  /* #CP  */
        [22] = 0,  /* reserved */
        [23] = 0,  /* reserved */
        [24] = 0,  /* reserved */
        [25] = 0,  /* reserved */
        [26] = 0,  /* reserved */
        [27] = 0,  /* reserved */
        [28] = 0,  /* #HV: no error code */
        [29] = 1,  /* #VC: carries the VMEXIT code */
        [30] = 1,  /* #SX: carries the security exception code */
        [31] = 0,  /* reserved */
    };
    axys_uint32_t vector;

    for (vector = 0; vector < 32; ++vector) {
        const axys_uint32_t bit = (AXYS_IRQ_ERROR_VECTORS >> vector) & 1u;
        assert(bit == authoritative[vector]);
    }

    /* Nothing above 31 is an exception, so nothing above 31 may be flagged --
     * a stray high bit would consume a word the CPU never pushed. */
    assert((AXYS_IRQ_ERROR_VECTORS >> 32) == 0ull);

    /*
     * 20 is the specific one to pin, because nothing in this kernel can raise
     * #VE: it is only delivered on a transition into a VMX or SVM guest, and
     * there is none. So no runtime check can ever catch a regression here, which
     * is why it needs a literal. If a future hypervisor or VMX support makes it
     * reachable, delete this assertion only after confirming the new path.
     */
    assert(((AXYS_IRQ_ERROR_VECTORS >> 20) & 1u) == 0u);
}

int main(void)
{
    test_rsp_is_above_the_frame();
    test_rsp_independent_of_vector();
    test_page_fault_decode();
    test_selector_codes_are_not_pwf_bits();
    test_double_fault_decode();
    test_codeless_vector_is_not_decoded();
    test_vendor_error_vectors();
    test_error_mask_matches_architecture();
    test_exception_names();
    return 0;
}
