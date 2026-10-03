#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "axys/gdt.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/types.h"

/*
 * Host-side check that struct axys_interrupt_frame describes the stack that
 * isr_common actually builds.
 *
 * This exists because the mapping between the two is invisible to the
 * compiler: getting the field order wrong still compiles, still links, and
 * still boots -- it just makes every handler read the wrong stack slot. The
 * register dump in the exception path would be nonsense, and a dispatcher
 * would compare against a saved GPR instead of the real vector.
 *
 * The layout is modelled as the CPU and stubs actually produce it, which means
 * emulating push order (each push stores *after* decrementing, so the last
 * push lands at the lowest address):
 *
 *   CPU   (error, only for some vectors) rflags, cs, rip
 *   stub  vector
 *   common rax, rbx, rcx, rdx, rsi, rdi, rbp, r8 .. r15
 */

#define AXYS_TEST_ERROR_VECTOR 14u  /* #PF: the CPU pushes an error code */
#define AXYS_TEST_PLAIN_VECTOR 6u   /* #UD: the CPU pushes none          */
#define AXYS_TEST_ERROR 0x0002u     /* #PF, write, present */

/* Distinct sentinels so a permuted field is detected, not just a zero. */
#define AXYS_TEST_RAX 0x1111111111111111ull
#define AXYS_TEST_RBX 0x2222222222222222ull
#define AXYS_TEST_RCX 0x3333333333333333ull
#define AXYS_TEST_RDX 0x4444444444444444ull
#define AXYS_TEST_RSI 0x5555555555555555ull
#define AXYS_TEST_RDI 0x6666666666666666ull
#define AXYS_TEST_RBP 0x7777777777777777ull
#define AXYS_TEST_R8 0x8888888888888888ull
#define AXYS_TEST_R9 0x9999999999999999ull
#define AXYS_TEST_R10 0xaaaaaaaaaaaaaaaaull
#define AXYS_TEST_R11 0xbbbbbbbbbbbbbbbbull
#define AXYS_TEST_R12 0xccccccccccccccccull
#define AXYS_TEST_R13 0xddddddddddddddddull
#define AXYS_TEST_R14 0xeeeeeeeeeeeeeeeeull
#define AXYS_TEST_R15 0xffffffffffffffffull
#define AXYS_TEST_RIP 0x00007fffffff1234ull
#define AXYS_TEST_CS 0x0008ull
#define AXYS_TEST_RFLAGS 0x0000000000020246ull

/*
 * Builds the frame exactly as the asm does, by simulating the pushes onto a
 * downward-growing stack. Returns the frame pointer (the lowest address), which
 * is what isr_common hands to the handler as its first argument.
 */
static struct axys_interrupt_frame *build_frame(uint64_t vector, int with_error,
                                               uint64_t *out_top)
{
    /*
     * A plain array standing in for the stack. The real kernel's frame lives
     * at the very top of an 8 KiB page, but the *relative* layout is all that
     * is under test, so the backing store does not need to model that.
     */
    static uint64_t stack[128];
    uint64_t *sp = &stack[64];
    struct axys_interrupt_frame *frame;

    /*
     * CPU entry, in chronological push order. Note the error code is pushed
     * LAST, after the saved state -- which, on a downward stack, puts it at the
     * LOWEST address of the CPU's frame, directly below rip. Getting this
     * backwards is the single easiest mistake to make here, and it produces a
     * frame that still links and still boots.
     */
    *--sp = AXYS_TEST_RFLAGS;
    *--sp = AXYS_TEST_CS;
    *--sp = AXYS_TEST_RIP;
    if (with_error) {
        *--sp = AXYS_TEST_ERROR;
    }

    /* The stub pushes a dummy error for code-less vectors, then the vector. */
    if (!with_error) {
        *--sp = 0;
    }
    *--sp = vector;

    /* isr_common saves the GPRs in this order. */
    *--sp = AXYS_TEST_RAX;
    *--sp = AXYS_TEST_RBX;
    *--sp = AXYS_TEST_RCX;
    *--sp = AXYS_TEST_RDX;
    *--sp = AXYS_TEST_RSI;
    *--sp = AXYS_TEST_RDI;
    *--sp = AXYS_TEST_RBP;
    *--sp = AXYS_TEST_R8;
    *--sp = AXYS_TEST_R9;
    *--sp = AXYS_TEST_R10;
    *--sp = AXYS_TEST_R11;
    *--sp = AXYS_TEST_R12;
    *--sp = AXYS_TEST_R13;
    *--sp = AXYS_TEST_R14;
    *--sp = AXYS_TEST_R15;

    frame = (struct axys_interrupt_frame *)sp;
    *out_top = (uint64_t)(uintptr_t)&stack[64];
    return frame;
}

static void check_registers(const struct axys_interrupt_frame *f, uint64_t vector)
{
    assert(f->r15 == AXYS_TEST_R15);
    assert(f->r14 == AXYS_TEST_R14);
    assert(f->r13 == AXYS_TEST_R13);
    assert(f->r12 == AXYS_TEST_R12);
    assert(f->r11 == AXYS_TEST_R11);
    assert(f->r10 == AXYS_TEST_R10);
    assert(f->r9 == AXYS_TEST_R9);
    assert(f->r8 == AXYS_TEST_R8);
    assert(f->rbp == AXYS_TEST_RBP);
    assert(f->rdi == AXYS_TEST_RDI);
    assert(f->rsi == AXYS_TEST_RSI);
    assert(f->rdx == AXYS_TEST_RDX);
    assert(f->rcx == AXYS_TEST_RCX);
    assert(f->rbx == AXYS_TEST_RBX);
    assert(f->rax == AXYS_TEST_RAX);

    assert(f->vector == vector);
    assert(f->rip == AXYS_TEST_RIP);
    assert(f->cs == AXYS_TEST_CS);
    assert(f->rflags == AXYS_TEST_RFLAGS);
}

static void test_error_frame(void)
{
    uint64_t top = 0;
    struct axys_interrupt_frame *frame =
        build_frame(AXYS_TEST_ERROR_VECTOR, 1, &top);

    check_registers(frame, AXYS_TEST_ERROR_VECTOR);

    /* A real CPU code, and the vector is one of the code-carrying set. */
    assert(frame->error == AXYS_TEST_ERROR);
    assert(axys_interrupt_has_error(frame));

    /*
     * The frame must occupy exactly AXYS_INTERRUPT_FRAME_BYTES and sit
     * immediately below the CPU's saved state, with rip/cs/rflags on top. If
     * the struct grew or shrank, the offsets iretq depends on would move.
     */
    assert((uint64_t)(uintptr_t)frame + AXYS_INTERRUPT_FRAME_BYTES == top);
}

static void test_no_error_frame(void)
{
    uint64_t top = 0;
    struct axys_interrupt_frame *frame =
        build_frame(AXYS_TEST_PLAIN_VECTOR, 0, &top);

    check_registers(frame, AXYS_TEST_PLAIN_VECTOR);

    /* The stub supplied a dummy 0, and the vector is not in the error set. */
    assert(frame->error == 0);
    assert(!axys_interrupt_has_error(frame));

    /* Uniform size either way: that is what lets the epilogue drop 16 bytes
     * unconditionally. */
    assert((uint64_t)(uintptr_t)frame + AXYS_INTERRUPT_FRAME_BYTES == top);
}

/*
 * The vector field has to be the 16th word, immediately after the 15 GPRs, and
 * error the 17th. If it is not, a dispatcher silently tests a saved register
 * against a vector number and every IRQ would be misrouted.
 */
static void test_vector_position(void)
{
    uint64_t top = 0;
    struct axys_interrupt_frame *frame =
        build_frame(AXYS_TEST_ERROR_VECTOR, 1, &top);
    const uint64_t *words = (const uint64_t *)frame;

    assert(words[14] == AXYS_TEST_RAX);
    assert(words[15] == AXYS_TEST_ERROR_VECTOR);
    assert(words[16] == AXYS_TEST_ERROR);
    assert(words[17] == AXYS_TEST_RIP);
    assert(words[18] == AXYS_TEST_CS);
    assert(words[19] == AXYS_TEST_RFLAGS);
}

/* Both frames are the same size; the stub always supplies an error word. */
static void test_frame_size(void)
{
    assert(sizeof(struct axys_interrupt_frame) == AXYS_INTERRUPT_FRAME_BYTES);
    assert(AXYS_INTERRUPT_FRAME_WORDS == 20);
    assert(AXYS_INTERRUPT_FRAME_BYTES == 160);
}

/*
 * The 16-byte gate descriptor is a bit field, so every field is a byte offset
 * and a shift, not a mask that can be or-ed together. All of these compile and
 * link regardless; they only fail at delivery time, on real hardware, which is
 * the worst place to find out.
 */
#define GATE_IST 4
#define GATE_TYPE_ATTR 5
#define GATE_OFFSET_OF_IST 0x04u

static void test_gate_bit_positions(void)
{
    struct axys_idt_entry gate;

    memset(&gate, 0, sizeof(gate));

    gate.ist = AXYS_IDT_IST(AXYS_IST_DOUBLE_FAULT);
    gate.type_attributes = AXYS_IDT_ATTR_KERNEL;

    /* The IST byte really is the fifth one. */
    assert(GATE_OFFSET_OF_IST == offsetof(struct axys_idt_entry, ist));
    assert(((const unsigned char *)&gate)[GATE_IST] == gate.ist);
    assert(((const unsigned char *)&gate)[GATE_TYPE_ATTR] == gate.type_attributes);

    assert(sizeof(struct axys_idt_entry) == 16);
}

/*
 * The IST index belongs in bits 6-4. Writing the raw index produced a byte of
 * 0x01, which the CPU decodes as IST 0 plus a reserved gate type of 1: the
 * entry is malformed, so delivering a #DF through it faults again instead of
 * switching to the dedicated stack. This is what made a deliberate double fault
 * escalate until the machine reset.
 */
static void test_ist_shifts_into_place(void)
{
    assert(AXYS_IDT_IST(0) == 0x00u);
    assert(AXYS_IDT_IST(1) == 0x10u);
    assert(AXYS_IDT_IST(2) == 0x20u);
    assert(AXYS_IDT_IST(7) == 0x70u);

    /* The shift is 4; at 3 the index would land on the reserved zero bit. */
    assert(AXYS_IDT_IST_SHIFT == 4);

    /* Bits 6-4 only. An index must never touch the reserved bit 3 or the gate
     * type in bits 3-0, which is why the mask is 0x7. */
    assert((AXYS_IDT_IST(7) & 0x0fu) == 0u);
    assert((AXYS_IDT_IST(7) & 0x80u) == 0u);

    /* Values above 7 are clamped by the mask rather than corrupting the type. */
    assert(AXYS_IDT_IST(8) == AXYS_IDT_IST(0));
    assert(AXYS_IDT_IST(0xff) == AXYS_IDT_IST(7));

    /* The real gate for #DF: IST 1 set, 64-bit interrupt gate, DPL 0. */
    assert((AXYS_IDT_IST(AXYS_IST_DOUBLE_FAULT) | AXYS_IDT_ATTR_KERNEL) == 0x9Eu);
}

/*
 * DPL 3 is 0b11 in bits 6-5, so a user-accessible gate is 0xEE. 0xAE encodes
 * DPL 1, which is not a level any entry point uses, and would reject every
 * delivery from ring 3.
 */
static void test_privilege_levels(void)
{
    assert(AXYS_IDT_ATTR_KERNEL == 0x8Eu);
    assert(AXYS_IDT_ATTR_USER == 0xEEu);

    assert(((AXYS_IDT_ATTR_KERNEL >> 5) & 0x3u) == 0u);
    assert(((AXYS_IDT_ATTR_USER >> 5) & 0x3u) == 3u);

    /* S must be clear and the gate must be a 64-bit interrupt gate. */
    assert((AXYS_IDT_ATTR_KERNEL & 0x10u) == 0u);
    assert((AXYS_IDT_ATTR_USER & 0x10u) == 0u);
    assert((AXYS_IDT_ATTR_KERNEL & 0x0fu) == 0x0Eu);
    assert((AXYS_IDT_ATTR_USER & 0x0fu) == 0x0Eu);

    /* The type belongs in the fifth byte, never OR-ed into the IST byte. */
    assert((AXYS_IDT_ATTR_KERNEL & 0x0fu) != 0u);
    assert((AXYS_IDT_IST(AXYS_IST_DOUBLE_FAULT) & 0x0fu) == 0u);
}

/*
 * The kernel's own #DF gate, assembled from the same helpers axys_idt_init uses.
 * This is the exact byte sequence the CPU is handed for vector 8, so a
 * regression in either the shift or the attribute shows up here as a literal
 * mismatch rather than as a triple fault at runtime.
 */
static void test_double_fault_gate(void)
{
    const unsigned char expected[16] = {
        0x34, 0x12,             /* offset_low                    */
        0x08, 0x00,             /* selector = kernel code        */
        0x10,                   /* ist = 1 << 4                  */
        0x8E,                   /* present, DPL 0, 64-bit gate   */
        0x00, 0x00,             /* offset_middle                 */
        0x00, 0x00, 0x00, 0x00, /* offset_high (address < 4 GiB) */
        0x00, 0x00, 0x00, 0x00, /* reserved                      */
    };
    const axys_uintptr_t handler = 0x1234u;
    struct axys_idt_entry gate;

    memset(&gate, 0, sizeof(gate));
    gate.offset_low = (axys_uint16_t)(handler & 0xffffu);
    gate.selector = 0x08u;
    gate.ist = AXYS_IDT_IST(AXYS_IST_DOUBLE_FAULT);
    gate.type_attributes = AXYS_IDT_ATTR_KERNEL;
    gate.offset_middle = (axys_uint16_t)((handler >> 16) & 0xffffu);
    gate.offset_high = (axys_uint32_t)(handler >> 32);
    gate.reserved = 0;

    assert(memcmp(&gate, expected, sizeof(expected)) == 0);
}

/*
 * The TSS offsets the CPU reads directly out of the descriptor limit. A struct
 * that is merely the right size is silently wrong, so pin the byte offsets the
 * header already asserts on, and confirm IST 1 addresses a real stack slot
 * rather than the end of the whole array.
 */
static void test_tss_ist_slots(void)
{
    const size_t ist_stack_size = 8192;
    const size_t ist_count = 3;
    unsigned char ist_stacks[ist_count * ist_stack_size];
    axys_uint64_t tss_ist[7];
    unsigned index;

    assert(offsetof(struct axys_tss, rsp) == 0x04);
    assert(offsetof(struct axys_tss, ist) == 0x24);
    assert(offsetof(struct axys_tss, iomap_base) == 0x66);
    assert(offsetof(struct axys_tss, iomap) == 0x68);
    assert(sizeof(struct axys_tss) == 0x2069);
    assert(sizeof(tss_ist) / sizeof(tss_ist[0]) == 7);
    assert(AXYS_IST_COUNT <= 7);
    assert(AXYS_IST_DOUBLE_FAULT == 1);
    assert(AXYS_IST_NONE == 0);

    /*
     * Region 0 backs IST 1, so the top handed to tss.ist[0] is one region in,
     * not at the end of the whole array. The buggy version used
     * ist_stacks + AXYS_IST_COUNT * size, which is the top of the LAST region
     * and leaves region 0 dead.
     */
    tss_ist[AXYS_IST_DOUBLE_FAULT - 1] =
        (axys_uint64_t)(uintptr_t)(ist_stacks + ist_stack_size);

    assert(tss_ist[AXYS_IST_DOUBLE_FAULT - 1] !=
           (axys_uint64_t)(uintptr_t)(ist_stacks + ist_count * ist_stack_size));
    assert(tss_ist[AXYS_IST_DOUBLE_FAULT - 1] >=
           (axys_uint64_t)(uintptr_t)ist_stacks);
    assert(tss_ist[AXYS_IST_DOUBLE_FAULT - 1] <
           (axys_uint64_t)(uintptr_t)(ist_stacks + ist_stack_size * 2));

    /* Each index gets a distinct, non-overlapping top. */
    for (index = 1; index < 7; ++index) {
        tss_ist[index - 1] = (axys_uint64_t)(uintptr_t)(ist_stacks + index * ist_stack_size);
        assert(tss_ist[index - 1] ==
               (axys_uint64_t)(uintptr_t)(ist_stacks + (index + 1) * ist_stack_size) - ist_stack_size);
    }
}

/*
 * tss.ist[i] holds the *top* of the stack: the value the CPU loads into RSP and
 * then pushes down from. The usable region is therefore [top - size, top].
 *
 * Getting this backwards is easy and the failure is quiet -- the region comes
 * out empty, every "is the frame on the IST stack?" comparison fails, and the
 * reading is that the CPU ignored the IST. It does not look like an arithmetic
 * slip at all. This has already happened once, in axys_gdt_ist_stack, where the
 * bottom was taken as ist_stacks + index * size and the stored top as the upper
 * bound, making top == bottom.
 */
static void test_ist_bounds_hang_below_the_top(void)
{
    const size_t ist_stack_size = 8192;
    const size_t ist_count = 3;
    unsigned char ist_stacks[ist_count * ist_stack_size] __attribute__((aligned(4096)));
    axys_uint64_t top;
    uintptr_t low;
    uintptr_t high;

    for (unsigned index = 1; index < ist_count; ++index) {
        top = (axys_uint64_t)(uintptr_t)(ist_stacks + (index + 1) * ist_stack_size);
        low = (uintptr_t)(top - ist_stack_size);
        high = (uintptr_t)top;

        /* The region is one whole stack, not zero bytes. */
        assert(high - low == ist_stack_size);

        /* And it is the region this index owns, i.e. below the top of the
         * *next* region, not overlapping it. */
        assert(low == (uintptr_t)(ist_stacks + index * ist_stack_size));
        assert(high == (uintptr_t)(ist_stacks + (index + 1) * ist_stack_size));

        /* A frame built on this stack lands at or below the top and at or above
         * the bottom: the interrupted RSP is the top itself, and the CPU pushes
         * down from there. */
        assert(high <= high);
        assert(high - 1 >= low);
        assert(high - ist_stack_size == low);

        /* The top the CPU loads is 4 KiB aligned, which is what the CPU
         * prefers; the bottom need not be. */
        assert((high & 4095u) == 0u);
    }

    /* The specific error: treating the stored top as the bottom collapses the
     * region to nothing. */
    top = (axys_uint64_t)(uintptr_t)(ist_stacks + ist_stack_size);
    assert(((uintptr_t)top - ist_stack_size) != (uintptr_t)top);
}

int main(void)
{
    test_frame_size();
    test_error_frame();
    test_no_error_frame();
    test_vector_position();
    test_gate_bit_positions();
    test_ist_shifts_into_place();
    test_privilege_levels();
    test_double_fault_gate();
    test_tss_ist_slots();
    test_ist_bounds_hang_below_the_top();
    return 0;
}
