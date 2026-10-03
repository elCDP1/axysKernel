#include "axys/gdt.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/string.h"

/* Code selector for the ring-0 long-mode code segment installed by
 * axys_gdt_init (index 1). Kept here so the IDT and GDT agree on one value. */
#define IDT_KERNEL_SELECTOR 0x08

/* Indexed by CPU vector. 256 entries covers every vector the CPU can raise,
 * including the ones this kernel has no handler logic for. Declared in
 * axys/interrupts.h, alongside the frame layout the stubs implement. */

static struct axys_idt_entry idt[AXYS_IDT_ENTRIES] __attribute__((aligned(16)));
static struct axys_idt_pointer idt_pointer;

void axys_idt_set_gate(axys_uint8_t vector, axys_uintptr_t handler,
                       axys_uint8_t dpl, axys_uint8_t ist_index)
{
    /* Reject malformed privilege/IST inputs instead of silently truncating
     * them into a different descriptor. DPL is 2 bits and an IST index is
     * the three-bit value 1..7 (0 disables IST). */
    if (dpl > 3 || ist_index > 7) {
        return;
    }

    /* Gate descriptors are little-endian bit fields, not a struct you can
     * memcpy. The CPU requires a canonical 64-bit handler address; callers
     * are responsible for passing an address in the kernel's canonical range. */
    idt[vector].offset_low = (axys_uint16_t)(handler & 0xffffu);
    idt[vector].selector = IDT_KERNEL_SELECTOR;
    idt[vector].ist = AXYS_IDT_IST(ist_index);
    idt[vector].type_attributes = AXYS_IDT_ATTR_KERNEL | (axys_uint8_t)(dpl << 5);
    idt[vector].offset_middle = (axys_uint16_t)((handler >> 16) & 0xffffu);
    idt[vector].offset_high = (axys_uint32_t)(handler >> 32);
    idt[vector].reserved = 0;
}

void axys_idt_set(axys_uint8_t vector, axys_uintptr_t handler,
                  axys_uint8_t type_attributes)
{
    idt[vector].offset_low = (axys_uint16_t)(handler & 0xffffu);
    idt[vector].selector = IDT_KERNEL_SELECTOR;
    idt[vector].ist = 0;
    idt[vector].type_attributes = type_attributes;
    idt[vector].offset_middle = (axys_uint16_t)((handler >> 16) & 0xffffu);
    idt[vector].offset_high = (axys_uint32_t)(handler >> 32);
    idt[vector].reserved = 0;
}

void axys_idt_set_ist(axys_uint8_t vector, axys_uintptr_t handler,
                      axys_uint8_t ist_index)
{
    axys_idt_set_gate(vector, handler, 0, ist_index);
}

void axys_idt_init(void)
{
    axys_uint32_t vector;

    axys_memset(idt, 0, sizeof(idt));
    axys_memset(&idt_pointer, 0, sizeof(idt_pointer));

    /*
     * Install a real, present gate for *every* vector.
     *
     * A not-present entry is worse than a useless one: the CPU turns any
     * interrupt aimed at it into #GP, and if the stack is already damaged that
     * becomes a triple fault with no diagnostics. With a gate installed,
     * axys_isr_handler gets a chance to print the vector and panic cleanly.
     */
    for (vector = 0; vector < AXYS_IDT_ENTRIES; ++vector) {
        axys_idt_set((axys_uint8_t)vector, axys_isr_stub_table[vector],
                     AXYS_IDT_ATTR_KERNEL);
    }

    /*
     * #DF (vector 8) gets a dedicated stack. A double fault usually *means*
     * the current stack overflowed, so running the handler on that same
     * stack is what turns a recoverable #DF into a triple fault. The IST
     * entry is only meaningful once axys_tss_load has run, which must
     * therefore happen before the first fault can be taken.
     *
     * axys_idt_set_ist has to shift the index into bits 6-4 of the gate's
     * fifth byte; see AXYS_IDT_IST. Storing the raw index there produces a
     * malformed gate that faults on delivery, which for #DF is an immediate
     * re-escalation rather than a recovery.
     */
    axys_idt_set_ist(8, axys_isr_stub_table[8], AXYS_IST_DOUBLE_FAULT);

    idt_pointer.limit = (axys_uint16_t)(sizeof(idt) - 1);
    for (axys_size_t index = 0; index < 8; ++index) {
        idt_pointer.base[index] =
            (axys_uint8_t)((axys_uintptr_t)idt >> (index * 8));
    }
    axys_idt_flush(&idt_pointer);
}

const struct axys_idt_entry *axys_idt_entry(axys_uint8_t vector)
{
    /*
     * No bounds check needed, and adding one is a trap: AXYS_IDT_ENTRIES is
     * 256 and the parameter is axys_uint8_t, so `vector >= 256` is false for
     * every representable value and the check is dead code that -Werror
     * rejects. The type is the bound. A wider parameter would need the check
     * again, and widening it just to test it would be the real mistake.
     */
    return &idt[vector];
}

const char *axys_exception_name(axys_uint32_t vector)
{
    /*
     * Designated by vector number, not by position. A plain positional list
     * silently mislabels everything after a missing entry, and it had exactly
     * that problem: the table held 31 strings for 32 vectors, so index 31 was
     * left NULL and every name from 22 upward was off by one. Printing NULL
     * happens to survive in the exception reporter (printf substitutes
     * "(null)"), which is why it survived so long -- but any caller that
     * compared or measured the result would fault.
     *
     * tests/test_exceptions.c walks all 32 vectors and requires a real name for
     * each, so a dropped initialiser fails the build rather than boot.
     */
    static const char *const names[AXYS_IDT_EXCEPTION_COUNT] = {
        [0] = "divide error",
        [1] = "debug",
        [2] = "non-maskable interrupt",
        [3] = "breakpoint",
        [4] = "overflow",
        [5] = "bound range exceeded",
        [6] = "invalid opcode",
        [7] = "device not available",
        [8] = "double fault",
        [9] = "coprocessor segment overrun",
        [10] = "invalid TSS",
        [11] = "segment not present",
        [12] = "stack-segment fault",
        [13] = "general protection fault",
        [14] = "page fault",
        [15] = "reserved (15)",
        [16] = "x87 floating-point exception",
        [17] = "alignment check",
        [18] = "machine check",
        [19] = "SIMD floating-point exception",
        [20] = "virtualization exception",
        [21] = "control protection exception",
        [22] = "reserved (22)",
        [23] = "reserved (23)",
        [24] = "reserved (24)",
        [25] = "reserved (25)",
        [26] = "reserved (26)",
        [27] = "reserved (27)",
        [28] = "hypervisor injection exception (AMD)",
        [29] = "VMM communication exception (AMD SEV-ES)",
        [30] = "security exception (AMD SVM)",
        [31] = "reserved (31)",
    };

    if (vector >= AXYS_IDT_EXCEPTION_COUNT) {
        return "unknown";
    }
    return names[vector];
}
