#include "axys/gdt.h"
#include "axys/string.h"

#define GDT_CODE_INDEX 1
#define GDT_DATA_INDEX 2
#define GDT_TSS_INDEX 5 /* occupies the 16-byte slot after entries[0..4] */
#define GDT_CODE_SELECTOR 0x08
#define GDT_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR 0x28

#define KERNEL_STACK_SIZE 16384
#define IST_STACK_SIZE 8192

/*
 * I/O permission bitmap geometry (Intel SDM Vol. 3, system-configuration
 * space): the bitmap starts at TSS.iomap_base, covers one bit per port up to
 * AXYS_TSS_IO_BITMAP_BYTES*8-1, and must be followed by a terminating 0xff
 * byte (that terminator is the extra byte in AXYS_TSS_IO_BITMAP_TOTAL_BYTES).
 * The TSS limit has to include the termination byte for the CPU to accept the
 * bitmap at all; anything beyond the covered range is treated as denied. All
 * ones therefore means "no ring-3 I/O anywhere".
 */
#define AXYS_IOMAP_LAST_BYTE     (offsetof(struct axys_tss, iomap) +      \
                                  AXYS_TSS_IO_BITMAP_BYTES)                 \
                                 /* index of the termination byte */
#define AXYS_TSS_LIMIT_WITH_IOMAP                                            \
    ((axys_uint32_t)(AXYS_IOMAP_LAST_BYTE))  /* == sizeof(tss) - 1: the whole
                                              * struct, bitmap included */

/*
 * Access bytes for the flat ring-0 model this kernel uses.
 *
 * Code: 0x9a = present, DPL 0, code, executable/readable.
 * Data: 0x92 = present, DPL 0, data, writable.
 *
 * Granularity/limit-high byte 0xaf sets G=1 (4 KiB granularity, so a limit of
 * 0xffff means 4 GiB) together with D/B=1. 0xcf does the same for data; D/B
 * must be 1 for a data segment, otherwise the CPU applies 16-bit stack
 * semantics and truncates RSP on a far return.
 */
#define GDT_CODE_FLAGS 0xaf
#define GDT_DATA_FLAGS 0xcf

/* 0x89 = present, DPL 0, available 64-bit TSS. */
#define GDT_TSS_ACCESS 0x89

/* 16-byte alignment: the GDT pointer is read with a single aligned access by
 * lgdt on many microarchitectures, and matching the IDT's alignment avoids a
 * split load in the hot interrupt path. */
static struct axys_gdt_image gdt_image __attribute__((aligned(16)));
static struct axys_tss tss __attribute__((aligned(16)));
static struct axys_gdt_pointer gdt_pointer;

static axys_uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

/*
 * Dedicated interrupt stacks. ist_stacks[0] is unused because IST indices are
 * 1-based (0 means "keep the current stack"). Each stack is 4 KiB aligned as
 * the architecture prefers, and consumed from the top down.
 */
static axys_uint8_t ist_stacks[AXYS_IST_COUNT * IST_STACK_SIZE]
    __attribute__((aligned(4096)));

static void gdt_set_entry(axys_size_t index, axys_uint32_t base,
                          axys_uint32_t limit, axys_uint8_t access,
                          axys_uint8_t limit_high_flags)
{
    gdt_image.entries[index].limit_low = (axys_uint16_t)(limit & 0xffffu);
    gdt_image.entries[index].base_low = (axys_uint16_t)(base & 0xffffu);
    gdt_image.entries[index].base_middle = (axys_uint8_t)((base >> 16) & 0xffu);
    gdt_image.entries[index].access = access;
    gdt_image.entries[index].limit_high_flags = limit_high_flags;
    gdt_image.entries[index].base_high = (axys_uint8_t)((base >> 24) & 0xffu);
}

static void gdt_set_tss(axys_uintptr_t base, axys_uint32_t limit)
{
    gdt_image.tss.limit_low = (axys_uint16_t)(limit & 0xffffu);
    gdt_image.tss.base_low = (axys_uint16_t)(base & 0xffffu);
    gdt_image.tss.base_middle = (axys_uint8_t)((base >> 16) & 0xffu);
    gdt_image.tss.access = GDT_TSS_ACCESS;
    gdt_image.tss.limit_high_flags = (axys_uint8_t)((limit >> 16) & 0x0fu);
    gdt_image.tss.base_high = (axys_uint8_t)((base >> 24) & 0xffu);
    gdt_image.tss.base_high_high = (axys_uint32_t)(base >> 32);
    gdt_image.tss.reserved = 0;
}

void axys_gdt_set_kernel_stack(void *stack_top)
{
    tss.rsp[0] = (axys_uint64_t)(axys_uintptr_t)stack_top;
}

void axys_tss_set_ist(axys_uint8_t index, void *stack_top)
{
    if (index == AXYS_IST_NONE || index >= AXYS_IST_COUNT) {
        return;
    }
    tss.ist[index - 1] = (axys_uint64_t)(axys_uintptr_t)stack_top;
}

int axys_gdt_ist_stack(axys_uint8_t index, axys_uintptr_t *low,
                       axys_uintptr_t *high)
{
    axys_uintptr_t top;

    if (index == AXYS_IST_NONE || index >= AXYS_IST_COUNT) {
        return 0;
    }
    top = (axys_uintptr_t)tss.ist[index - 1];
    if (top == 0) {
        return 0;
    }

    /*
     * tss.ist[index-1] holds the *top* of the stack -- the address the CPU loads
     * into RSP and then pushes down from. So the usable region is
     * [top - IST_STACK_SIZE, top], and the bottom is only derivable by
     * subtracting the size. Treating the stored value as the bottom instead
     * makes every comparison come out empty, which reads as "no IST
     * programmed" no matter how correctly the table was filled in.
     */
    if (top < IST_STACK_SIZE ||
        top - IST_STACK_SIZE < (axys_uintptr_t)ist_stacks ||
        top > (axys_uintptr_t)ist_stacks + sizeof(ist_stacks)) {
        return 0;
    }
    if (low != 0) {
        *low = top - IST_STACK_SIZE;
    }
    if (high != 0) {
        *high = top;
    }
    return 1;
}

axys_uint64_t axys_tss_ist_raw(axys_uint8_t index)
{
    if (index == AXYS_IST_NONE || index >= AXYS_IST_COUNT) {
        return 0;
    }
    return tss.ist[index - 1];
}

void axys_gdt_init(void)
{
    axys_memset(&gdt_image, 0, sizeof(gdt_image));
    axys_memset(&tss, 0, sizeof(tss));
    axys_memset(&gdt_pointer, 0, sizeof(gdt_pointer));
    axys_memset(ist_stacks, 0, sizeof(ist_stacks));

    /* Flat segments: base 0, limit 0xffff with G=1 covers the full 4 GiB.
     * (The stray comment that used to sit here described IST stacks, which
     * have nothing to do with segment limits.) */
    gdt_set_entry(GDT_CODE_INDEX, 0, 0xffffu, 0x9a, GDT_CODE_FLAGS);
    gdt_set_entry(GDT_DATA_INDEX, 0, 0xffffu, 0x92, GDT_DATA_FLAGS);
    /* Ring-3 segments, in the order SYSRET requires: with STAR[63:48] = 0x10,
     * SYSRET loads SS = 0x10+8|3 (user data) and CS = 0x10+16|3 (user code). */
    gdt_set_entry(3, 0, 0xffffu, 0xf2, GDT_DATA_FLAGS); /* user data, DPL 3 */
    gdt_set_entry(4, 0, 0xffffu, 0xfa, GDT_CODE_FLAGS); /* user code, DPL 3, L=1 */
    /* The TSS limit must cover the fixed fields *and* the I/O permission
     * bitmap plus its terminating byte; a limit that stops at sizeof(tss)-1
     * or at the fixed fields makes the CPU treat the bitmap as absent and
     * #GP on every ring-3 port access. */
    gdt_set_tss((axys_uintptr_t)&tss, AXYS_TSS_LIMIT_WITH_IOMAP);

    /*
     * iomap_base must point at the I/O permission bitmap, and the TSS limit
     * must include it. With the bitmap all ones the CPU denies every port
     * access at CPL > 0; since only ring 0 exists today, it is inert but
     * correct for when user mode arrives. The bitmap is relevant to accesses
     * made with CPL > IOPL; ring 0 code is still governed by normal IOPL
     * rules, so the bitmap does not deny ring-0 port I/O by itself.
     */
    /* Architectural minimum for iomap_base: the Intel SDM requires the I/O
     * permission bitmap base to be at least 0x68 (the end of the fixed TSS
     * fields). A smaller value means the CPU never finds a valid bitmap and
     * raises #GP on *every* port access made with CPL > IOPL -- a trap that
     * only bites once ring 3 exists, so it must be pinned down now, before
     * user mode can depend on it. The layout itself is already asserted in
     * axys/gdt.h (offsetof(iomap) == 0x68); the clamp below enforces the same
     * minimum at runtime, which today is a no-op but stays correct if the
     * struct is ever reordered or shrunk. */
    {
        axys_uint32_t io_base = (axys_uint32_t)offsetof(struct axys_tss, iomap);

        if (io_base < 0x68u) {
            io_base = 0x68u;
        }
        tss.iomap_base = (axys_uint16_t)io_base;
    }

    /*
     * Default policy: deny every port to ring 3. The bitmap is filled with
     * ones for the full range it covers and for the mandatory terminating
     * byte (the TSS limit was sized above to include exactly these bytes).
     * Ports beyond the covered range are denied architecturally, so the
     * effective rule is "no user-mode I/O" until a task abstraction grants
     * per-task port permissions. Ring-0 code is unaffected: it runs at
     * CPL 0 <= IOPL and the bitmap is only consulted for accesses made with
     * CPL > IOPL.
     */
    axys_memset(tss.iomap, 0xffu, AXYS_TSS_IO_BITMAP_TOTAL_BYTES);

    gdt_pointer.limit = (axys_uint16_t)(sizeof(gdt_image) - 1);
    for (axys_size_t index = 0; index < 8; ++index) {
        gdt_pointer.base[index] =
            (axys_uint8_t)((axys_uintptr_t)&gdt_image >> (index * 8));
    }

    /*
     * Populate the IST and the ring-0 stack *before* the ltr. A double fault
     * between ltr and this point would land on an IST slot that is still 0,
     * which is itself a #GP.
     *
     * Each IST index gets the top of its *own* slot. ist_stacks is laid out
     * as AXYS_IST_COUNT consecutive IST_STACK_SIZE regions with index 0 unused
     * (IST indices are 1-based), so index N is served by region N-1 and its
     * top is (N-1+1)*IST_STACK_SIZE. Pointing every index at the end of the
     * whole array -- as an earlier version did for the double fault -- made
     * IST 1 start at the top of region 2, silently overlapping whichever stack
     * owned that region.
     */
    axys_gdt_set_kernel_stack(kernel_stack + KERNEL_STACK_SIZE);
    {
        axys_uint8_t index;

        for (index = AXYS_IST_NONE + 1; index < AXYS_IST_COUNT; ++index) {
            axys_tss_set_ist(index, ist_stacks + (axys_size_t)index * IST_STACK_SIZE);
        }
    }

    axys_gdt_flush(&gdt_pointer);
    axys_tss_load();
}
