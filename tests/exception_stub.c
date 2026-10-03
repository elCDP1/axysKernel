#include "axys/exceptions.h"
#include "axys/gdt.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/types.h"

/*
 * Host-side link stubs for tests/test_exceptions.c.
 *
 * kernel/exceptions.c is written for bare metal, so the only thing standing
 * between it and a host test is the handful of hardware and assembly symbols it
 * reaches for. None of them are on the path the decoder test exercises, so they
 * are stubbed here rather than dragging in the real CPU, TSS and IDT
 * assembly -- which would need the interrupt stubs, which need the dispatcher,
 * which needs the frame this test is trying to check in isolation.
 */

void axys_panic(const char *message)
{
    (void)message;
    __builtin_trap();
}

void axys_panic_format(const char *format, ...)
{
    (void)format;
    __builtin_trap();
}

/* The decoder calls these only for #PF: the I/D-bit redaction and CR2. A host
 * CPUID call would be the real answer, but the redaction only matters on parts
 * that advertise it, and reporting 0 is the conservative branch. */
int axys_cpu_has_nx(void)
{
    return 0;
}

axys_uint64_t axys_cpu_read_cr2(void)
{
    return 0;
}

axys_uint64_t axys_cpu_read_cr3(void)
{
    return 0;
}

axys_uint64_t axys_cpu_read_cr4(void)
{
    return 0;
}

/* The fault generators live in arch/x86_64/fault.S and trap by design, so they
 * cannot run on the host. axys_fault_test_run must not reach them: the test
 * drives axys_exception_decode directly. */
void axys_fault_ud2(void) {}
void axys_fault_breakpoint(void) {}
void axys_fault_divide(void) {}
void axys_fault_page(void) {}
void axys_fault_double(void) {}
void axys_fault_ist(void) {}

/*
 * The IST wiring lives in kernel/gdt.c, which is unreachable from a host test:
 * it needs `lgdt`, `ltr` and a real TSS. axys_exception_selftest reads the
 * tss.ist slots and the IST stack bounds, so those two accessors are stubbed to
 * report "no IST" -- which is the honest answer for a host that has no TSS, and
 * the value the self test already knows how to report.
 *
 * Note axys_cpu_read_tr returns 0 here, which the self test treats as "ltr
 * loaded no TSS". That is exactly the state on a host, so the diagnostic path
 * is consistent; only axys_exception_decode is under test in this binary.
 */
int axys_gdt_ist_stack(axys_uint8_t index, axys_uintptr_t *low,
                       axys_uintptr_t *high)
{
    (void)index;
    if (low != 0) {
        *low = 0;
    }
    if (high != 0) {
        *high = 0;
    }
    return 0;
}

axys_uint64_t axys_tss_ist_raw(axys_uint8_t index)
{
    (void)index;
    return 0;
}

axys_uint16_t axys_cpu_read_tr(void)
{
    return 0;
}

/* Referenced by kernel/idt.c when it installs the table wholesale. The values are
 * never dereferenced in this test. */
axys_uintptr_t axys_isr_stub_table[AXYS_IDT_ENTRIES];

void axys_idt_flush(const struct axys_idt_pointer *pointer)
{
    (void)pointer;
}

/*
 * The real implementation lives in kernel/interrupts.c and compares
 * AXYS_IRQ_ERROR_VECTORS against axys_error_vector_table, which the assembly
 * emits. There is no assembly here, so the check cannot run -- the host test
 * verifies the mask itself against an independently written table instead
 * (see test_error_mask_matches_architecture in tests/test_exceptions.c).
 */
unsigned axys_interrupt_layout_mismatches(void)
{
    return 0;
}

/* kernel/exceptions.c kills faulting user processes; no ring 3 on the host. */
#include "axys/process.h"
void axys_process_fault(const struct axys_interrupt_frame *frame)
{
    (void)frame;
    __builtin_trap();
}
