#include "axys/cpu.h"

/* GCC's "memory" clobber keeps the compiler from caching anything across these
 * asm blocks. asm volatile alone would not: the compiler is otherwise free to
 * assume memory does not change behind its back, which is exactly the wrong
 * assumption for a register write the hardware acts on immediately. */
#define CPU_BARRIER __asm__ volatile("" ::: "memory")

void axys_cpu_relax(void)
{
    __asm__ volatile("pause");
}

void axys_cpu_halt(void)
{
    /*
     * Halt until the next interrupt, leaving IF alone.
     *
     * This must NOT contain a cli. The kernel's idle loop is
     * "sti; for (;;) halt;" and a cli here would clear IF on the first
     * iteration and never restore it, so the machine would look healthy while
     * silently servicing nothing.
     */
    __asm__ volatile("hlt");
}

void axys_cpu_idle(void)
{
    /*
     * The canonical idle instruction when IF is set: sti lets any pending
     * interrupt through immediately, hlt parks the core, and the arriving
     * interrupt wakes it. Avoids the race in "sti; hlt" where an interrupt
     * arriving between the two instructions is delayed by one idle period.
     */
    __asm__ volatile("sti; hlt");
}

void axys_cpu_enable_interrupts(void)
{
    __asm__ volatile("sti" ::: "memory");
}

void axys_cpu_disable_interrupts(void)
{
    __asm__ volatile("cli" ::: "memory");
}

int axys_cpu_interrupts_enabled(void)
{
    axys_uint64_t flags;

    /* Pushfq/pop captures IF along with everything else; bit 9 is IF. */
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return (int)((flags >> 9) & 1u);
}

axys_uint64_t axys_cpu_save_flags(void)
{
    axys_uint64_t flags;

    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}

void axys_cpu_restore_flags(axys_uint64_t flags)
{
    /*
     * The whole of RFLAGS comes back, IF included -- which is the point, and is
     * why this takes a value from axys_cpu_save_flags rather than a 0/1
     * "interrupts were on" boolean. An earlier version tested bit 0 (carry) and
     * took a 0/1 flag, so it happened to work for its one caller and would have
     * silently mis-restored real RFLAGS from any other source.
     *
     * "cc" is clobbered because popfq can change the arithmetic flags.
     */
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

axys_uint64_t axys_cpu_read_cr2(void)
{
    axys_uint64_t value;

    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

axys_uint64_t axys_cpu_read_cr3(void)
{
    axys_uint64_t value;

    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

axys_uint64_t axys_cpu_read_cr4(void)
{
    axys_uint64_t value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

void axys_cpu_write_cr4(axys_uint64_t value)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

axys_uint16_t axys_cpu_read_tr(void)
{
    axys_uint16_t selector;

    /*
     * `str` stores the task-register selector. There is no way to read the TSS
     * descriptor's *base* back out, so the selector is the only directly
     * observable evidence that `ltr` took effect -- which matters because a
     * silent ltr failure leaves the TR as 0 and makes every IST field in the
     * IDT inert, with the handlers still working perfectly. Nothing else in the
     * kernel can tell the difference.
     *
     * `str` is valid in 64-bit mode and, unlike `sgdt`/`sidt`, is not
     * privileged.
     */
    __asm__ volatile("str %0" : "=r"(selector));
    return selector;
}

void axys_cpu_write_cr3(axys_uint64_t value)
{
    /*
     * CR3 writes need a full serialising effect, otherwise a subsequent
     * access to a TLB entry can be satisfied from a stale translation. The
     * "mov cr3" itself is serialising on all known parts, but the compiler
     * must be told not to reorder memory operations across it.
     */
    __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
    CPU_BARRIER;
}

axys_uint64_t axys_cpu_read_msr(axys_uint32_t msr)
{
    axys_uint32_t low;
    axys_uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((axys_uint64_t)high << 32) | low;
}

void axys_cpu_write_msr(axys_uint32_t msr, axys_uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((axys_uint32_t)value),
                       "d"((axys_uint32_t)(value >> 32)));
    CPU_BARRIER;
}

void axys_cpu_cpuid(axys_uint32_t leaf, axys_uint32_t subleaf,
                    axys_uint32_t *out_eax, axys_uint32_t *out_ebx,
                    axys_uint32_t *out_ecx, axys_uint32_t *out_edx)
{
    /*
     * RBX is a reserved register in 32-bit mode but ordinary in 64-bit, so
     * this is a long-mode-only helper. The separate "=b" output constraint is
     * what tells the compiler rbx is clobbered; without it GCC may keep a live
     * value there across the instruction.
     */
    __asm__ volatile("cpuid"
                     : "=a"(*out_eax), "=b"(*out_ebx), "=c"(*out_ecx),
                       "=d"(*out_edx)
                     : "a"(leaf), "c"(subleaf));
}

axys_uint32_t axys_cpu_leaf_max(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    axys_cpu_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

axys_uint32_t axys_cpu_extended_leaf_max(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    axys_cpu_cpuid(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    return eax;
}

int axys_cpu_has_rdrand(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 1) {
        return 0;
    }
    axys_cpu_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ecx >> 30) & 1u);
}

int axys_cpu_has_rdseed(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    /*
     * RDSEED is CPUID leaf 7, subleaf 0, ECX[18] -- a *basic* leaf. An earlier
     * version range-checked the extended max (0x80000007) instead, which says
     * nothing about whether leaf 7 exists: a CPU with basic max 1 and a full
     * extended range would sail past the guard and cpuid a leaf that returns
     * whatever the previous query happened to leave behind.
     */
    if (axys_cpu_leaf_max() < 7) {
        return 0;
    }
    axys_cpu_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ebx >> 18) & 1u);
}

int axys_cpu_has_rdtscp(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    /*
     * Must range-check the extended leaf first. Executing CPUID with a leaf
     * above the reported maximum returns whatever the last query left behind
     * on some CPUs, so an unchecked read can report a feature as present (or
     * absent) for no reason at all.
     */
    if (axys_cpu_extended_leaf_max() < 0x80000001u) {
        return 0;
    }
    axys_cpu_cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
    return (int)((edx >> 27) & 1u);
}

int axys_cpu_has_nx(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_extended_leaf_max() < 0x80000001u) {
        return 0;
    }
    axys_cpu_cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
    return (int)((edx >> 20) & 1u);
}

axys_uint64_t axys_cpu_enable_smep_smap(void)
{
    axys_uint64_t cr4 = axys_cpu_read_cr4();
    axys_uint64_t set = 0;

    if (axys_cpu_has_smep()) {
        set |= AXYS_CR4_SMEP;
    }
    if (axys_cpu_has_smap()) {
        set |= AXYS_CR4_SMAP;
    }
    if (set != 0) {
        axys_cpu_write_cr4(cr4 | set);
    }
    return set;
}

int axys_cpu_has_smep(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 7) {
        return 0;
    }
    axys_cpu_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ebx >> 7) & 1u);
}

int axys_cpu_has_smap(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 7) {
        return 0;
    }
    axys_cpu_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ebx >> 20) & 1u);
}

int axys_cpu_has_fsgsbase(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 7) {
        return 0;
    }
    axys_cpu_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ebx >> 0) & 1u);
}

int axys_cpu_has_apic(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 1) {
        return 0;
    }
    axys_cpu_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (int)((edx >> 9) & 1u);
}

int axys_cpu_has_x2apic(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_leaf_max() < 1) {
        return 0;
    }
    axys_cpu_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    return (int)((ecx >> 21) & 1u);
}

int axys_cpu_has_1gb_pages(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_extended_leaf_max() < 0x80000001u) {
        return 0;
    }
    axys_cpu_cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
    return (int)((edx >> 26) & 1u);
}

int axys_cpu_has_invariant_tsc(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;

    if (axys_cpu_extended_leaf_max() < 0x80000007u) {
        return 0;
    }
    axys_cpu_cpuid(0x80000007u, 0, &eax, &ebx, &ecx, &edx);
    return (int)((edx >> 8) & 1u);
}

axys_uint32_t axys_cpu_count(void)
{
    axys_uint32_t eax;
    axys_uint32_t ebx;
    axys_uint32_t ecx;
    axys_uint32_t edx;
    axys_uint32_t max_leaf = axys_cpu_leaf_max();
    axys_uint32_t count = 1;

    /* CPUID.0B reports the logical-processor count at each topology level.
     * The highest non-zero EBX value is the package-wide logical count on the
     * standard SMT/core hierarchy; unlike subleaf 0 alone, it is not merely
     * the number of threads per core. */
    if (max_leaf >= 0x0bu) {
        for (axys_uint32_t subleaf = 0; subleaf < 32u; ++subleaf) {
            axys_cpu_cpuid(0x0bu, subleaf, &eax, &ebx, &ecx, &edx);
            if ((ebx & 0xffffu) == 0 || ((ecx >> 8) & 0xffu) == 0) {
                break;
            }
            if ((ebx & 0xffffu) > count) {
                count = ebx & 0xffffu;
            }
        }
        if (count > 1) {
            return count;
        }
    }

    if (max_leaf >= 1) {
        axys_cpu_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
        if ((edx & (1u << 28)) != 0) {
            axys_uint32_t logical = (ebx >> 16) & 0xffu;
            if (logical != 0) {
                return logical;
            }
        }
    }
    return 1;
}
