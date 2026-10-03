#ifndef AXYS_IDT_H
#define AXYS_IDT_H

#include "axys/types.h"

/*
 * A 16-byte IA-32e gate descriptor. Note the CPU requires the high 32 bits of
 * a 64-bit interrupt handler address to be sign-consistent; every address in
 * this kernel is below 4 GiB so offset_high is always zero.
 */
struct axys_idt_entry {
    axys_uint16_t offset_low;
    axys_uint16_t selector;
    axys_uint8_t ist;            /* already shifted: (index << 4), 0 = current stack */
    axys_uint8_t type_attributes;
    axys_uint16_t offset_middle;
    axys_uint32_t offset_high;
    axys_uint32_t reserved;
} __attribute__((packed));

struct axys_idt_pointer {
    axys_uint16_t limit;
    axys_uint8_t base[8];
} __attribute__((packed));

#define AXYS_IDT_ENTRIES          256
#define AXYS_IDT_EXCEPTION_COUNT  32
#define AXYS_IDT_IRQ_COUNT        16
#define AXYS_IDT_IRQ_VECTOR_BASE  32
#define AXYS_IDT_IRQ_VECTOR_END   (AXYS_IDT_IRQ_VECTOR_BASE + AXYS_IDT_IRQ_COUNT)

/*
 * Type/attribute bytes. These are complete byte values, not masks: the CPU
 * decodes them positionally, so `present | dpl | gate_type | ist`.
 *
 *   bit 7     P  -- present
 *   bits 6-5  DPL -- descriptor privilege level (0 = kernel only)
 *   bit 4     S  -- must be 0 for an interrupt/trap gate in long mode
 *   bits 3-0  type -- 0xE interrupt gate (clears IF), 0xF trap gate
 *
 * DPL 3 encodes as 0b11 in bits 6-5, so 0x80|0x60|0x0E == 0xEE. Writing 0xAE
 * (as an earlier version did) encodes DPL 1, which is not a privilege level
 * that exists for any entry point and would reject every user-mode delivery.
 */
#define AXYS_IDT_ATTR_KERNEL   0x8Eu /* present, DPL 0, 64-bit interrupt gate */
#define AXYS_IDT_ATTR_USER     0xEEu /* as above but DPL 3: user-callable
                                      * interrupt gate (NOT a syscall ABI --
                                      * SYSCALL/SYSRET or int 0x80 semantics
                                      * are designed separately) */

/*
 * The IST index lives in bits 6-4 of the gate's *fifth byte* (offset 4), not in
 * bits 2-0. Bit 3 of that byte is a reserved zero and bits 3-0 are the gate
 * type, so an index of 1 has to be encoded as 0x10. Storing the raw index makes
 * the byte 0x01, which decodes as IST 0 with a reserved gate type of 1: the
 * entry is malformed, so the CPU faults on delivery instead of switching to the
 * dedicated stack -- which is exactly how a mis-wired #DF becomes a triple
 * fault. Always assign through AXYS_IDT_IST.
 */
#define AXYS_IDT_IST_SHIFT     4
#define AXYS_IDT_IST(ist)      (((ist) & 0x7u) << AXYS_IDT_IST_SHIFT)

void axys_idt_init(void);
void axys_idt_set(axys_uint8_t vector, axys_uintptr_t handler,
                  axys_uint8_t type_attributes);
void axys_idt_set_ist(axys_uint8_t vector, axys_uintptr_t handler,
                      axys_uint8_t ist_index);
void axys_idt_set_gate(axys_uint8_t vector, axys_uintptr_t handler,
                       axys_uint8_t dpl, axys_uint8_t ist_index);
void axys_idt_flush(const struct axys_idt_pointer *pointer);

/* Human-readable name for a CPU exception vector, or "unknown". */
const char *axys_exception_name(axys_uint32_t vector);

/*
 * The installed gate for `vector`, or 0 if `vector` is out of range.
 *
 * Read-only on purpose: the encoded bytes are the only thing the CPU sees, and
 * a gate whose IST byte is wrong is still a structurally valid gate. So the
 * self test asserts over these bytes directly instead of over the intent of
 * the code that installed them.
 */
const struct axys_idt_entry *axys_idt_entry(axys_uint8_t vector);

#endif
