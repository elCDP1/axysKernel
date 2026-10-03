#include "axys/pagetest.h"

#include "axys/printf.h"
#include "axys/string.h"
#include "axys/types.h"

/*
 * Boot-time self-test for the initial page tables built by arch/x86_64/boot.S.
 *
 * Host tests compile none of boot.S, so a paging-encoding mistake there (the
 * historical NX-in-bit-31 bug is exactly such a mistake) is invisible until
 * the machine triple-faults. This test runs in the C kernel, after long mode
 * is live, and walks the *actual* structures boot.S installed -- reading them
 * through their own identity mapping -- to prove:
 *
 *   1. PML4[0] -> pdpt_table, PDPT[0] -> pd_table0 (identity chain intact).
 *   2. The first 2 MiB is mapped page-by-page with W^X:
 *        .text   present, supervisor, writable=0, NX=0 (read/execute)
 *        .rodata present, supervisor, writable=0, NX=1
 *        .data/.bss present, supervisor, writable=1, NX=1
 *      and every entry's physical field equals its virtual address.
 *   3. Page 0 and the boot-stack guard page are NOT present.
 *   4. The remaining low-GiB 2 MiB PDEs carry PS|RW|US-style flags with NX in
 *      bit 63 only -- never a stray 0x80000000 in the low half (which would
 *      silently shift the mapping up by 2 GiB).
 *
 * It must run before the PMM trusts memory, because a wrong mapping makes any
 * allocator statistic meaningless.
 */

#define ENTRY_PRESENT 0x1ull
#define ENTRY_WRITABLE 0x2ull
#define ENTRY_USER 0x4ull
#define ENTRY_PSE 0x80ull
#define ENTRY_NX 0x8000000000000000ull
#define ENTRY_ADDR_MASK 0x000FFFFFFFFFF000ull

extern char pml4_table[];
extern char pdpt_table[];
extern char pd_table0[];
extern char pt_low_table[];
extern char stack_guard[];
extern char __kernel_start[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];

static unsigned checks_run;
static unsigned checks_failed;

static void expect(const char *what, int condition)
{
    ++checks_run;
    if (!condition) {
        ++checks_failed;
        axys_printf("pagetest: FAIL %s\n", what);
    }
}

static axys_uint64_t *table_at(void *table)
{
    return (axys_uint64_t *)table; /* identity-mapped: address == physical */
}

static int entry_maps(axys_uint64_t entry, axys_uint64_t expected_phys)
{
    return (entry & ENTRY_ADDR_MASK) == expected_phys;
}

int axys_pagetest_selftest(void)
{
    axys_uint64_t *pml4 = table_at(pml4_table);
    axys_uint64_t *pdpt = table_at(pdpt_table);
    axys_uint64_t *pd0 = table_at(pd_table0);
    axys_uint64_t *pt0 = table_at(pt_low_table);
    axys_uint64_t text_start = (axys_uint64_t)(axys_uintptr_t)__text_start;
    axys_uint64_t text_end = (axys_uint64_t)(axys_uintptr_t)__text_end;
    axys_uint64_t rodata_start = (axys_uint64_t)(axys_uintptr_t)__rodata_start;
    axys_uint64_t rodata_end = (axys_uint64_t)(axys_uintptr_t)__rodata_end;
    axys_uint64_t data_start = (axys_uint64_t)(axys_uintptr_t)__data_start;
    axys_uint64_t bss_end = (axys_uint64_t)(axys_uintptr_t)__bss_end;
    axys_uint64_t guard = (axys_uint64_t)(axys_uintptr_t)stack_guard;
    axys_uint64_t v;

    checks_run = 0;
    checks_failed = 0;

    expect("PML4[0] present", (pml4[0] & ENTRY_PRESENT) != 0);
    expect("PML4[0] points at pdpt_table",
           entry_maps(pml4[0], (axys_uint64_t)(axys_uintptr_t)pdpt_table));
    expect("PDPT[0] present", (pdpt[0] & ENTRY_PRESENT) != 0);
    expect("PDPT[0] points at pd_table0",
           entry_maps(pdpt[0], (axys_uint64_t)(axys_uintptr_t)pd_table0));

    /* Fine-grained first 2 MiB. */
    expect("PDE[0] has no PS flag (4 KiB split)", (pd0[0] & ENTRY_PSE) == 0);
    expect("PDE[0] points at pt_low_table",
           entry_maps(pd0[0], (axys_uint64_t)(axys_uintptr_t)pt_low_table));
    expect("page 0 not present (NULL faults)", (pt0[0] & ENTRY_PRESENT) == 0);

    for (v = 0x1000; v < 0x200000; v += 0x1000) {
        axys_uint64_t e = pt0[v >> 12];
        const char *kind = "";
        int writable_expected = 1;
        int nx_expected = 1;

        if (v == guard) {
            expect("guard page not present", (e & ENTRY_PRESENT) == 0);
            continue;
        }
        if (v >= (text_start & ~0xFFFull) && v < ((text_end + 0xFFF) & ~0xFFF)) {
            kind = ".text";
            writable_expected = 0;
            nx_expected = 0;
        } else if (v >= (rodata_start & ~0xFFFull) && v < ((rodata_end + 0xFFF) & ~0xFFF)) {
            kind = ".rodata";
            writable_expected = 0;
        } else if (v >= (data_start & ~0xFFFull) && v < ((bss_end + 0xFFF) & ~0xFFF)) {
            kind = ".data/.bss";
        }

        {
            char label[48];
            axys_snprintf(label, sizeof(label), "%s page @0x%x flags", kind,
                          (unsigned)v);
            expect(label,
                   (e & ENTRY_PRESENT) != 0 &&
                   !(e & ENTRY_USER) &&
                   entry_maps(e, v) &&
                   ((e & ENTRY_WRITABLE) != 0) == (writable_expected != 0) &&
                   ((e & ENTRY_NX) != 0) == (nx_expected != 0) &&
                   (e & 0x80000000ull) == 0 /* no NX alias in the low half */);
        }
    }

    /* Remaining low-GiB entries are 2 MiB pages: PS set, RW+present, NX in
     * bit 63, physical field equal to the virtual address. */
    for (v = 2; v < 512; ++v) {
        axys_uint64_t e = pd0[v];
        char label[48];

        axys_snprintf(label, sizeof(label), "2MiB PDE[%u] flags", (unsigned)v);
        expect(label,
               (e & ENTRY_PRESENT) != 0 &&
               (e & ENTRY_PSE) != 0 &&
               (e & ENTRY_WRITABLE) != 0 &&
               !(e & ENTRY_USER) &&
               entry_maps(e, v * 0x200000ull) &&
               (e & ENTRY_NX) != 0 &&
               (e & 0x80000000ull) == 0);
    }

    expect("kernel image inside first 2 MiB mapping",
           (axys_uint64_t)(axys_uintptr_t)__kernel_end <= 0x200000ull);

    if (checks_failed != 0) {
        axys_printf("pagetest: FAILED (%u/%u checks)\n", checks_failed, checks_run);
        return -1;
    }
    axys_printf("pagetest: page tables verified (%u checks)\n", checks_run);
    return 0;
}
