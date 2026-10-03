#ifndef AXYS_GDT_H
#define AXYS_GDT_H

#include <stddef.h>
#include "axys/types.h"

#define AXYS_TSS_IO_BITMAP_BYTES 8192u
#define AXYS_TSS_IO_BITMAP_TOTAL_BYTES (AXYS_TSS_IO_BITMAP_BYTES + 1u)

struct axys_gdt_entry {
    axys_uint16_t limit_low;
    axys_uint16_t base_low;
    axys_uint8_t base_middle;
    axys_uint8_t access;
    axys_uint8_t limit_high_flags;
    axys_uint8_t base_high;
};

/*
 * The 64-bit Task State Segment, in the exact layout the CPU expects.
 *
 *   0x00  reserved (4)
 *   0x04  rsp[0..2]   ring 0/1/2 stack pointers (24 bytes)
 *   0x1c  reserved (8)
 *   0x24  ist[0..6]   interrupt stack pointers (56 bytes)
 *   0x5c  reserved (4)
 *   0x60  reserved (4)
 *   0x64  reserved (2)
 *   0x66  iomap_base  I/O bitmap offset (2 bytes)
 *   0x68  iomap       65536 port bits + required terminator byte
 *
 * `packed` is required because the 64-bit fields begin at non-8-byte offsets.
 * A normally aligned C struct would silently insert padding and break the IST
 * addresses read by hardware.
 */
struct axys_tss {
    axys_uint32_t reserved0;  /* 0x00 */
    axys_uint64_t rsp[3];     /* 0x04 */
    axys_uint64_t reserved1;  /* 0x1c */
    axys_uint64_t ist[7];     /* 0x24 */
    axys_uint32_t reserved2;  /* 0x5c */
    axys_uint32_t reserved3;  /* 0x60 */
    axys_uint16_t reserved4;  /* 0x64 */
    axys_uint16_t iomap_base; /* 0x66 */
    /* The final byte is the required all-ones I/O-map terminator. */
    axys_uint8_t iomap[AXYS_TSS_IO_BITMAP_TOTAL_BYTES]; /* 0x68 */
} __attribute__((packed));

/*
 * Interrupt Stack Table indices. Each is a 1-based selector into tss.ist[];
 * index 0 means "no IST", i.e. keep using the current stack.
 *
 * #DF is the important one: a double fault typically means the normal stack
 * overflowed or was corrupted, so running the handler on that same stack
 * guarantees a triple fault. A dedicated stack makes the common case
 * recoverable and at worst a clean panic.
 */
#define AXYS_IST_NONE   0
#define AXYS_IST_DOUBLE_FAULT 1
#define AXYS_IST_COUNT  3

/* A TSS occupies a full 16-byte slot in the GDT, not 8 bytes. Loading a
 * selector whose 16-byte system descriptor extends past the GDT limit raises
 * #GP with the selector value as the error code. */
struct axys_tss_gate {
    axys_uint16_t limit_low;
    axys_uint16_t base_low;
    axys_uint8_t base_middle;
    axys_uint8_t access;
    axys_uint8_t limit_high_flags;
    axys_uint8_t base_high;
    axys_uint32_t base_high_high;
    axys_uint32_t reserved;
} __attribute__((packed));

struct axys_gdt_image {
    struct axys_gdt_entry entries[5];
    struct axys_tss_gate tss;
} __attribute__((packed, aligned(8)));

struct axys_gdt_pointer {
    axys_uint16_t limit;
    axys_uint8_t base[8];
};

/* ist must point at a buffer of at least AXYS_IST_COUNT stack slots, each
 * already 4 KiB aligned. The stacks are consumed top-down. */
void axys_gdt_init(void);
void axys_gdt_set_kernel_stack(void *stack_top);

/* Selector values shared by the syscall/ring-3 entry code. */
#define AXYS_SEL_KERNEL_CODE 0x08u
#define AXYS_SEL_KERNEL_DATA 0x10u
#define AXYS_SEL_USER_DATA 0x1bu /* index 3, RPL 3 */
#define AXYS_SEL_USER_CODE 0x23u /* index 4, RPL 3 */
void axys_tss_set_ist(axys_uint8_t index, void *stack_top);
void axys_gdt_flush(const struct axys_gdt_pointer *pointer);
void axys_tss_load(void);

/*
 * The stack currently programmed for IST `index`, as a half-open range
 * [low, high). Returns 0 if `index` is out of range or nothing is programmed.
 *
 * This exists so the boot self test can check that a frame really was built on
 * the dedicated stack. That check is the only way to tell working IST wiring
 * from plausible-looking wiring, because a wrong IST index is ignored by the
 * CPU rather than rejected: the gate stays valid, the handler still runs, and
 * the only symptom is that it ran on the current stack instead of the reserved
 * one -- so a #DF meant to be recovered still escalates.
 */
int axys_gdt_ist_stack(axys_uint8_t index, axys_uintptr_t *low,
                       axys_uintptr_t *high);

/*
 * The raw tss.ist[index - 1] slot, or 0 if the index is invalid.
 *
 * Exposed separately from axys_gdt_ist_stack because that function answers
 * "is this a usable stack", and a 0 there conflates "never programmed" with
 * "programmed with something out of range". When IST delivery silently does
 * not happen, the slot's raw value is the first thing worth knowing.
 */
axys_uint64_t axys_tss_ist_raw(axys_uint8_t index);

/* These offsets are fixed by the architecture. Getting any of them wrong makes
 * the CPU ignore the IST or read the I/O bitmap from the wrong place. */
AXYS_STATIC_ASSERT(offsetof(struct axys_tss, rsp) == 0x04, tss_rsp_offset);
AXYS_STATIC_ASSERT(offsetof(struct axys_tss, ist) == 0x24, tss_ist_offset);
AXYS_STATIC_ASSERT(offsetof(struct axys_tss, iomap_base) == 0x66, tss_iomap_base_offset);
AXYS_STATIC_ASSERT(offsetof(struct axys_tss, iomap) == 0x68, tss_iomap_offset);
AXYS_STATIC_ASSERT(sizeof(struct axys_tss) == 0x68 + AXYS_TSS_IO_BITMAP_TOTAL_BYTES, tss_size);
AXYS_STATIC_ASSERT(AXYS_IST_COUNT <= 7, tss_ist_capacity);

#endif
