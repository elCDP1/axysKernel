#ifndef AXYS_CPU_H
#define AXYS_CPU_H

#include "axys/types.h"

/*
 * Control registers and CPU-state helpers.
 *
 * Note the deliberate split between axys_cpu_halt (which does *not* touch IF)
 * and axys_cpu_idle. An earlier version of this header folded cli into
 * axys_cpu_halt, which meant the kernel's "sti; for(;;) halt;" idle loop
 * disabled interrupts again on its very first iteration and stayed that way
 * forever -- the kernel looked alive but no interrupt would ever be serviced.
 */

void axys_cpu_relax(void);
void axys_cpu_halt(void);
void axys_cpu_idle(void);
void axys_cpu_enable_interrupts(void);
void axys_cpu_disable_interrupts(void);

/* Returns non-zero if interrupts would be serviced immediately, i.e. IF is
 * currently set. A query only -- it does not change IF. */
int axys_cpu_interrupts_enabled(void);

/* Save/restore the full RFLAGS, IF included. This is the correct pair for
 * bracketing a critical section:
 *
 *     axys_uint64_t flags = axys_cpu_save_flags();
 *     axys_cpu_disable_interrupts();
 *     ... critical section ...
 *     axys_cpu_restore_flags(flags);
 *
 * The restore is a straight popfq, so a value that did not come from
 * axys_cpu_save_flags is not a meaningful thing to pass here. */
axys_uint64_t axys_cpu_save_flags(void);
void axys_cpu_restore_flags(axys_uint64_t flags);

axys_uint64_t axys_cpu_read_cr2(void);
axys_uint64_t axys_cpu_read_cr3(void);
axys_uint64_t axys_cpu_read_cr4(void);
void axys_cpu_write_cr4(axys_uint64_t value);

#define AXYS_CR4_SMEP (1u << 20) /* block kernel-mode execution of user pages */
#define AXYS_CR4_SMAP (1u << 21) /* block kernel-mode access to user pages    */

/* Turn on SMEP/SMAP in CR4 for every bit CPUID says the CPU actually has
 * (detecting them is not the same as enabling them -- CR4 starts with both
 * clear). Safe to call more than once. Returns the bits it set. */
axys_uint64_t axys_cpu_enable_smep_smap(void);

/* Task-register selector, i.e. the GDT selector the TSS was loaded from. 0 means
 * no `ltr` has happened. The TSS base itself cannot be read back directly, so
 * this is the simplest observable check that the task-state segment is live. */
axys_uint16_t axys_cpu_read_tr(void);

void axys_cpu_write_cr3(axys_uint64_t value);

/* Read a model-specific register. Used for TSC and for EFER checks. */
axys_uint64_t axys_cpu_read_msr(axys_uint32_t msr);
void axys_cpu_write_msr(axys_uint32_t msr, axys_uint64_t value);

/* CPUID. Leaf 0 returns the highest supported leaf in *out_eax. */
void axys_cpu_cpuid(axys_uint32_t leaf, axys_uint32_t subleaf,
                    axys_uint32_t *out_eax, axys_uint32_t *out_ebx,
                    axys_uint32_t *out_ecx, axys_uint32_t *out_edx);

/* CPUID leaf 0x80000000+ extents and features.
 *
 * Every feature probe below range-checks the relevant maximum first. CPUID on a
 * leaf above the reported maximum returns stale contents on some CPUs, so an
 * unguarded read can report a feature as present or absent for no reason. */
axys_uint32_t axys_cpu_leaf_max(void);
axys_uint32_t axys_cpu_extended_leaf_max(void);
int axys_cpu_has_rdrand(void);
int axys_cpu_has_rdseed(void);
int axys_cpu_has_rdtscp(void);
int axys_cpu_has_smep(void);
int axys_cpu_has_smap(void);
/* NX support: CPUID.80000001:EDX[20]. When NX is enabled in EFER, #PF error
 * code bit 4 is the instruction-fetch flag. */
int axys_cpu_has_nx(void);
int axys_cpu_has_fsgsbase(void);
int axys_cpu_has_apic(void);
int axys_cpu_has_x2apic(void);
int axys_cpu_has_1gb_pages(void);

/* Invariant-TSC: lets code trust rdtsc as a monotonic counter. */
int axys_cpu_has_invariant_tsc(void);

/* Logical processor count, from CPUID 0xb when present and the legacy leaf 1
 * topology fields otherwise. This is a query only: no other CPU is started and
 * the APIC is left alone, so the kernel still runs on the boot processor until
 * SMP bring-up exists. */
axys_uint32_t axys_cpu_count(void);

#define AXYS_MSR_EFER 0xc0000080u
#define AXYS_MSR_IA32_TSC 0x10u
#define AXYS_MSR_IA32_TSC_AUX 0x19au

#define AXYS_EFER_SCE (1u << 0)  /* System Call Extensions      */
#define AXYS_EFER_LME (1u << 8)  /* Long Mode Enable           */
#define AXYS_EFER_LMA (1u << 10) /* Long Mode Active           */
#define AXYS_EFER_NXE (1u << 11) /* No-Execute pages enabled   */

#endif
