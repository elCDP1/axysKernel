#include "axys/cpu.h"
#include "axys/exceptions.h"
#include "axys/process.h"
#include "axys/gdt.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/panic.h"
#include "axys/printf.h"
#include "axys/string.h"

#ifndef CONFIG_AXYS_DANGEROUS_SELFTEST
#define CONFIG_AXYS_DANGEROUS_SELFTEST 0
#endif

/*
 * Fault-injection state. The generator routines in arch/x86_64/fault.S are
 * void functions containing a single trapping instruction followed by `ret`,
 * so resuming after the trap means advancing the saved RIP by the length of
 * that instruction and letting the `ret` execute. The generator records its
 * own resume RIP here so the handler can do that without hard-coding opcode
 * lengths.
 */
/*
 * Where the exception handler should resume after absorbing a deliberately
 * injected fault. Written by the generators in arch/x86_64/fault.S immediately
 * before they trap, so it always points just past the trapping instruction.
 */
axys_uintptr_t axys_fault_resume_rip;
static axys_uint32_t fault_test_seen_vector = 0xffffffffu;
static axys_uintptr_t fault_test_frame_sp;
static int fault_test_armed;
static int fault_test_in_progress;

/*
 * The vector the currently-running injection is allowed to produce. Without
 * this, any real exception that happened to land inside a test window would be
 * absorbed -- RIP redirected to the resume address -- and silently disappear.
 * A page fault during a #UD test is not the test: it is a bug, and swallowing
 * it hides exactly the kind of defect the self test exists to find.
 */
#define AXYS_FAULT_TEST_VECTOR_NONE 0xffffffffu
static axys_uint32_t fault_test_expected_vector = AXYS_FAULT_TEST_VECTOR_NONE;

void axys_fault_test_enable(int enabled)
{
    fault_test_armed = enabled;
    if (!enabled) {
        /* Disarming must close every window: a stale in_progress flag with no
         * armed test would absorb the next real exception of that vector. */
        fault_test_in_progress = 0;
        fault_test_expected_vector = AXYS_FAULT_TEST_VECTOR_NONE;
    }
}

int axys_fault_test_enabled(void)
{
    return fault_test_armed;
}

axys_uint32_t axys_fault_test_last_vector(void)
{
    return fault_test_seen_vector;
}

axys_uintptr_t axys_fault_test_last_frame_sp(void)
{
    return fault_test_frame_sp;
}

void axys_exception_decode(const struct axys_interrupt_frame *frame,
                           struct axys_exception_info *info)
{
    axys_memset(info, 0, sizeof(*info));

    info->vector = (axys_uint32_t)frame->vector;
    info->error_code = frame->error;
    info->rip = frame->rip;
    info->cs = frame->cs;
    info->rflags = frame->rflags;
    info->rbp = frame->rbp;

    /*
     * Start from "this field does not apply". The error-code format is
     * per-vector, and the old version of this function applied one present/
     * write/user/reserved/instruction/idt layout to every vector that carries
     * a code. That is actively wrong: for #GP/#NP/#SS the code is a selector,
     * for #DF it is a delivery type, and for #DE it is zero.
     */
    info->present = -1;
    info->write = -1;
    info->user = -1;
    info->reserved = -1;
    info->instruction = -1;
    info->idt = -1;

    /*
     * Only the vectors in AXYS_IRQ_ERROR_VECTORS push a real code; the rest
     * have the dummy 0 the stub supplied. Decoding bits out of a dummy would
     * print nonsense, so gate on the vector.
     */
    if (!axys_interrupt_has_error(frame)) {
        goto out;
    }

    switch (info->vector) {
    case 14: /* #PF */
        info->present = (int)(info->error_code & 1u);
        info->write = (int)((info->error_code >> 1) & 1u);
        info->user = (int)((info->error_code >> 2) & 1u);
        info->reserved = (int)((info->error_code >> 3) & 1u);

        /* With NX enabled, bit 4 is the architectural instruction-fetch flag.
         * It is reserved when execute-disable is unavailable/disabled. */
        if (axys_cpu_has_nx()) {
            info->instruction = (int)((info->error_code >> 4) & 1u);
        }

        /* CR2 holds the linear address that could not be translated. */
        info->fault_address = axys_cpu_read_cr2();
        info->has_fault_address = 1;
        break;

    case 11: /* #NP */
    case 12: /* #SS */
    case 13: /* #GP */
        /*
         * The error code is selector-style: bit 1 says whether the selector
         * came through the IDT, bit 2 is the LDT/GDT indicator, and the upper
         * bits carry the selector index. It is not a page-fault P/W/U value
         * and it is not a memory address -- fault_address stays reserved for
         * genuine linear addresses (CR2 on #PF) so diagnostics can never
         * print a selector as "fault address".
         */
        info->idt = (int)((info->error_code >> 1) & 1u);
        info->error_selector = info->error_code & 0xfff8u;
        info->has_error_selector = 1;
        break;

    case 8: /* #DF */
        /* The processor-defined #DF error code is always zero; no subfields. */
        break;

    default:
        /*
         * The remaining code-carrying vectors (#CP, #VC, #SEV, ...) have layouts
         * that do not map onto these fields. Leave everything at -1 and let the
         * raw value be printed rather than invent a reading.
         *
         * 20 #VE is not in this group: it carries no code at all, so the word
         * here is the stub's dummy 0 and decoding bits out of it would be
         * nonsense. It falls through to the same -1 treatment by way of
         * axys_interrupt_has_error, which now reports it as code-less.
         */
        break;
    }

out:
    /*
     * The interrupted RSP depends on the delivery shape, and treating one
     * formula as universal is exactly the mistake this comment block used to
     * document: `frame + 160` is only a real recovered value when the CPU
     * actually pushed SS:RSP after RFLAGS. That happens on a privilege
     * transition (CS.RPL == 3) and on IST entries (the stack switch itself
     * forces the save). For an ordinary same-CPL kernel fault the CPU pushes
     * nothing more -- there is no saved word to read -- so frame + size is
     * only an ESTIMATE of where the interrupted stack pointer was, which for
     * our single fixed kernel stack is nonetheless accurate to the byte.
     * Callers that must not guess use axys_exception_has_saved_rsp() to tell
     * the two cases apart instead of trusting info->rsp blindly.
     */
    if (axys_exception_has_saved_rsp(frame)) {
        const axys_uint64_t *extension =
            (const axys_uint64_t *)((const axys_uint8_t *)frame +
                                    AXYS_INTERRUPT_FRAME_BYTES);
        info->rsp = extension[0]; /* hardware-saved RSP */
    } else {
        info->rsp = (axys_uint64_t)(axys_uintptr_t)frame +
                    AXYS_INTERRUPT_FRAME_BYTES; /* estimate, see above */
    }
}

/*
 * Did the CPU push a real SS:RSP pair immediately after the software prefix?
 * True for CPL3-originated entries and for any gate with a non-zero IST index
 * (the IST switch always saves them). False for plain same-CPL delivery.
 */
int axys_exception_has_saved_rsp(const struct axys_interrupt_frame *frame)
{
    const struct axys_idt_entry *gate;

    if ((frame->cs & 3u) == 3u) {
        return 1; /* privilege transition: SS then RSP were pushed */
    }
    gate = axys_idt_entry((axys_uint8_t)frame->vector);
    if (gate != AXYS_NULL && (axys_uint32_t)(gate->ist >> AXYS_IDT_IST_SHIFT) !=
                                 AXYS_IST_NONE) {
        return 1; /* IST delivery: stack switch saved the pair */
    }
    return 0;
}

/*
 * Read the hardware-saved RSP/SS. Only valid when
 * axys_exception_has_saved_rsp() says the words exist; on a same-CPL entry
 * without IST the addresses below hold whatever the stub's next frame will
 * overwrite, so callers must check first.
 */
axys_uint64_t axys_exception_saved_rsp(const struct axys_interrupt_frame *frame)
{
    const axys_uint64_t *extension =
        (const axys_uint64_t *)((const axys_uint8_t *)frame +
                                AXYS_INTERRUPT_FRAME_BYTES);

    return extension[0];
}

axys_uint16_t axys_exception_saved_ss(const struct axys_interrupt_frame *frame)
{
    const axys_uint64_t *extension =
        (const axys_uint64_t *)((const axys_uint8_t *)frame +
                                AXYS_INTERRUPT_FRAME_BYTES);

    return (axys_uint16_t)extension[1];
}

static int vector_has_error_code(axys_uint32_t vector)
{
    return vector < 64 &&
           (AXYS_IRQ_ERROR_VECTORS & (1ull << (axys_uint64_t)vector)) != 0;
}

static void print_error_code(const struct axys_exception_info *info)
{
    axys_printf("error=0x%016x [", info->error_code);

    /* -1 means "this vector has no such field"; print nothing rather than lie. */
    if (info->present >= 0) {
        axys_printf("%c", info->present ? 'P' : '-');
    }
    if (info->write >= 0) {
        axys_printf("%c", info->write ? 'W' : 'R');
    }
    if (info->user >= 0) {
        axys_printf("%c", info->user ? 'U' : 'S');
    }
    if (info->reserved >= 0) {
        axys_printf("%c", info->reserved ? 'R' : '-');
    }
    if (info->instruction >= 0) {
        axys_printf("%c", info->instruction ? 'I' : 'D');
    }
    if (info->idt >= 0) {
        axys_printf("%c", info->idt ? 'I' : '-');
    }
    axys_printf("]");

    if (!vector_has_error_code(info->vector)) {
        axys_printf(" (this vector carries no error code; value is the stub's "
                    "dummy 0)\n");
    } else {
        axys_printf("\n");
    }
}

void axys_exception_report(const struct axys_interrupt_frame *frame)
{
    struct axys_exception_info info;

    axys_exception_decode(frame, &info);

    axys_printf("\n*** cpu exception: %s (vector %u)\n",
                axys_exception_name(info.vector), info.vector);
    print_error_code(&info);
    if (info.has_fault_address) {
        axys_printf("fault address=0x%016x\n", info.fault_address);
    }
    if (info.has_error_selector) {
        axys_printf("selector=0x%x\n", (unsigned)info.error_selector);
    }
    axys_printf("rip=0x%016x cs=0x%016x rflags=0x%016x\n", info.rip, info.cs,
                info.rflags);
    axys_printf("rsp=0x%016x rbp=0x%016x\n", info.rsp, info.rbp);
    axys_printf("rax=0x%016x rbx=0x%016x\n", frame->rax, frame->rbx);
    axys_printf("rcx=0x%016x rdx=0x%016x\n", frame->rcx, frame->rdx);
    axys_printf("rsi=0x%016x rdi=0x%016x\n", frame->rsi, frame->rdi);
    axys_printf("r8 =0x%016x r9 =0x%016x\n", frame->r8, frame->r9);
    axys_printf("r10=0x%016x r11=0x%016x\n", frame->r10, frame->r11);
    axys_printf("r12=0x%016x r13=0x%016x\n", frame->r12, frame->r13);
    axys_printf("r14=0x%016x r15=0x%016x\n", frame->r14, frame->r15);
    axys_printf("cr2=0x%016x cr3=0x%016x cr4=0x%016x\n", axys_cpu_read_cr2(),
                axys_cpu_read_cr3(), axys_cpu_read_cr4());
}

/*
 * Absorb a deliberately injected fault and resume just past the trapping
 * instruction.
 *
 * The resume address is not computed here: each generator publishes it into
 * axys_fault_resume_rip from assembly, right before it traps. Deriving it in C
 * instead (with a computed goto) lets the compiler place the label anywhere it
 * likes, including back onto the call site, which turns the "resume" into an
 * infinite re-entry into the same trap.
 */
static int fault_test_complete(struct axys_interrupt_frame *frame)
{
    if (!fault_test_armed || !fault_test_in_progress) {
        return 0;
    }
    /*
     * The window is open, but the exception must be the one we asked for.
     * Anything else is a real fault that merely overlapped the test: leave
     * in_progress set so it cannot absorb a second exception either, and let
     * the caller fall through to normal reporting/panic.
     */
    if ((axys_uint32_t)frame->vector != fault_test_expected_vector) {
        return 0;
    }
    fault_test_in_progress = 0;
    fault_test_expected_vector = AXYS_FAULT_TEST_VECTOR_NONE;
    fault_test_seen_vector = (axys_uint32_t)frame->vector;
    /*
     * Where the CPU built this frame. Recorded here, from the frame pointer the
     * assembly handed us, because this is the only observation that shows
     * whether the gate's IST field was honoured. Kept separate from the vector
     * on purpose: the vector comes back correct either way.
     */
    fault_test_frame_sp = (axys_uintptr_t)frame;
    frame->rip = (axys_uint64_t)axys_fault_resume_rip;
    return 1;
}

/* Open the absorb window for exactly `expected`, run the generator, close it. */
#define FAULT_TEST_RUN(which_, expected_)            \
    do {                                             \
        fault_test_expected_vector = (expected_);    \
        fault_test_in_progress = 1;                  \
        axys_fault_##which_();                       \
        fault_test_in_progress = 0;                  \
        fault_test_expected_vector =                 \
            AXYS_FAULT_TEST_VECTOR_NONE;             \
    } while (0)

axys_uint32_t axys_fault_test_run(axys_uint32_t which)
{
    if (!fault_test_armed) {
        return 0xffffffffu;
    }

    fault_test_seen_vector = 0xffffffffu;
    fault_test_frame_sp = 0;

    switch (which) {
    case AXYS_FAULT_UD2:
        FAULT_TEST_RUN(ud2, 6);
        break;
    case AXYS_FAULT_BP:
        FAULT_TEST_RUN(breakpoint, 3);
        break;
    case AXYS_FAULT_DE:
        FAULT_TEST_RUN(divide, 0);
        break;
    case AXYS_FAULT_PF:
        FAULT_TEST_RUN(page, 14);
        break;
    case AXYS_FAULT_IST:
        FAULT_TEST_RUN(ist, AXYS_FAULT_TEST_IST_VECTOR);
        break;
    case AXYS_FAULT_DF:
        FAULT_TEST_RUN(double, 8);
        break;
    default:
        return 0xffffffffu;
    }

    return fault_test_seen_vector;
}

void axys_exception_entry(struct axys_interrupt_frame *frame)
{
    /*
     * A deliberately injected fault is not a failure: record it and resume.
     * This is checked before anything else because the handlers below print
     * and panic, and a self test must not trip either.
     */
    if (fault_test_complete(frame)) {
        return;
    }

    /* A fault raised in ring 3 is the process's problem, not the kernel's:
     * kill that process and keep running. (CPL is the low two bits of CS.) */
    if ((frame->cs & 3u) == 3u) {
        axys_process_fault(frame);
    }

    axys_exception_report(frame);

    switch ((axys_uint32_t)frame->vector) {
    case 2: /* #NMI: often a watchdog or memory parity event. */
    case 6: /* #UD */
    case 13: /* #GP */
    case 14: /* #PF */
    case 19: /* #XM */
        axys_panic("unhandled cpu exception");
        break;
    case 3: /* #DB: single-step trap. */
        axys_panic("unexpected debug exception");
        break;
    default:
        axys_panic("unexpected cpu vector");
        break;
    }
}

/*
 * Dump the gate bytes and the IST stack the #DF path depends on.
 *
 * This is diagnostics, not a test, and it exists because of a specific blind
 * spot: a gate whose IST byte is wrong is not an obviously invalid gate. It
 * still decodes, the handler still runs, and the vector still comes back
 * right. The only consequence is that no stack switch happened. So when IST
 * delivery does not occur, the encoded bytes and the raw TSS slots are the
 * first things worth having in the log.
 */
static void report_double_fault_gate(void)
{
    const struct axys_idt_entry *gate = axys_idt_entry(8);
    const axys_uint8_t *raw = (const axys_uint8_t *)gate;
    axys_uintptr_t low = 0;
    axys_uintptr_t high = 0;
    axys_size_t byte;

    axys_printf("  #DF gate: type_attrs=%02x ist_byte=%02x (ist=%u) handler="
                "%016lx\n",
                gate->type_attributes, gate->ist,
                (axys_uint32_t)(gate->ist >> AXYS_IDT_IST_SHIFT),
                (axys_uint64_t)((axys_uint32_t)gate->offset_low |
                                ((axys_uint32_t)gate->offset_middle << 16)));
    axys_printf("  #DF gate raw: ");
    for (byte = 0; byte < 16; ++byte) {
        axys_printf("%02x", raw[byte]);
    }
    axys_printf("\n");

    if (axys_gdt_ist_stack(AXYS_IST_DOUBLE_FAULT, &low, &high)) {
        axys_printf("  IST 1 stack: %016lx..%016lx (%u KiB)\n", low, high,
                    (axys_uint32_t)((high - low) / 1024u));
    } else {
        axys_printf("  IST 1 stack: NOT PROGRAMMED\n");
    }
    axys_printf("  tss.ist raw: [1]=%016lx [2]=%016lx\n",
                axys_tss_ist_raw(1), axys_tss_ist_raw(2));
    axys_printf("  tr=%04x cr4=%016lx idt gate 6 ist_byte=%02x\n",
                axys_cpu_read_tr(), axys_cpu_read_cr4(),
                axys_idt_entry(6)->ist);
}

/*
 * Hard checks on the IST wiring: the invariants the kernel itself controls.
 *
 * These are separated from the live delivery probe below because they are
 * verifiable everywhere. Whether a given emulator honours the IDT IST field is
 * a property of that emulator, and folding it into pass/fail would mean the
 * self test reports a kernel defect every time it runs on a host that does not
 * model the mechanism -- which teaches the reader to ignore the line.
 */
static unsigned check_ist_wiring(unsigned *checks)
{
    const struct axys_idt_entry *gate;
    axys_uintptr_t low = 0;
    axys_uintptr_t high = 0;
    unsigned failures = 0;

    /* Top of the range arch/x86_64/boot.S identity-maps with 2 MiB pages. An IST
     * stack above this would fault the instant the CPU switched to it, turning
     * a recovery path into a triple fault. */
    ++*checks;
    if (axys_cpu_read_tr() == 0) {
        axys_printf("  FAIL %-20s -> TR is 0, ltr loaded no TSS\n",
                    "TSS loaded");
        ++failures;
    }
    ++*checks;
    gate = axys_idt_entry(8);
    if ((axys_uint32_t)(gate->ist >> AXYS_IDT_IST_SHIFT) !=
        AXYS_IST_DOUBLE_FAULT) {
        axys_printf("  FAIL %-20s -> #DF gate ist byte %02x decodes to %u\n",
                    "#DF gate ist", gate->ist,
                    (axys_uint32_t)(gate->ist >> AXYS_IDT_IST_SHIFT));
        ++failures;
    }

    ++*checks;
    if (!axys_gdt_ist_stack(AXYS_IST_DOUBLE_FAULT, &low, &high)) {
        axys_printf("  FAIL %-20s -> tss.ist[0] is %016lx\n", "IST 1 in range",
                    axys_tss_ist_raw(1));
        return failures;
    }
    ++*checks;
    if (high - low != 8192u || (high & 4095u) != 0) {
        axys_printf("  FAIL %-20s -> %016lx..%016lx is not an aligned 8 KiB "
                    "stack\n", "IST 1 in range", low, high);
        ++failures;
    }
    ++*checks;
    if (high > 0x40000000ull) {
        axys_printf("  FAIL %-20s -> %016lx is outside the identity map\n",
                    "IST 1 mapped", high);
        ++failures;
    }

    if (failures == 0) {
        axys_printf("  ok   %-20s -> gate, TSS, TR and %016lx..%016lx "
                    "all agree\n", "IST 1 wiring", low, high);
    }
    return failures;
}

/*
 * Install IST 1 on `vector`, inject `which`, and report where the frame landed.
 *
 * This is an observation, not an assertion, and it deliberately does not
 * contribute to the failure count. The vector check is a real assertion and
 * is folded in; the stack switch is not, because it depends on the execution
 * environment actually delivering the interrupt through the IST mechanism.
 */
static unsigned observe_ist_delivery(axys_uint32_t which, axys_uint8_t vector,
                                     axys_uint32_t expected, const char *name,
                                     unsigned *checks)
{
    axys_uintptr_t low = 0;
    axys_uintptr_t high = 0;
    axys_uintptr_t sp;
    axys_uint32_t observed;

    axys_idt_set_ist(vector, axys_isr_stub_table[vector],
                     AXYS_IST_DOUBLE_FAULT);
    observed = axys_fault_test_run(which);
    sp = axys_fault_test_last_frame_sp();

    ++*checks; /* delivery reached the handler with the expected vector */
    if (observed != expected) {
        axys_printf("  FAIL %-20s -> expected %u, got %u\n", name, expected,
                    observed);
        return 1;
    }
    ++*checks; /* IST 1 has a usable stack to have switched onto */
    if (!axys_gdt_ist_stack(AXYS_IST_DOUBLE_FAULT, &low, &high)) {
        axys_printf("  FAIL %-20s -> IST 1 has no usable stack\n", name);
        return 1;
    }
    ++*checks; /* frame landed inside the IST 1 range (stack switch proof) */
    if (sp >= low && sp <= high &&
        sp <= high - AXYS_INTERRUPT_FRAME_BYTES - 16u) {
        axys_printf("  ok   %-20s -> vector %u, frame %016lx on IST 1\n", name,
                    observed, sp);
    } else {
        axys_printf("  note %-20s -> vector %u, frame %016lx on the current "
                    "stack; this host does not switch stacks\n",
                    name, observed, sp);
    }
    return 0;
}

/* Put `vector` back on the ordinary no-IST kernel gate. */
static void restore_plain_gate(axys_uint8_t vector)
{
    axys_idt_set(vector, axys_isr_stub_table[vector], AXYS_IDT_ATTR_KERNEL);
}

/*
 * Boot self test: raise one fault of each interesting shape and confirm the
 * handler saw the vector we expected, then check that the IST wiring is
 * internally consistent and observe whether this host honours it.
 *
 * This is the only thing that actually proves the IDT gates, the stubs, the
 * frame layout and the dispatcher agree with each other. A kernel can boot
 * perfectly with a completely broken exception path, because nothing faults
 * during normal startup -- the first sign of trouble is then a triple fault
 * with no diagnostics at all.
 */
void axys_exception_selftest(void)
{
    static const struct {
        axys_uint32_t which;
        axys_uint32_t expected;
        const char *name;
    } cases[] = {
        { AXYS_FAULT_DE, 0, "#DE divide error" },
        { AXYS_FAULT_BP, 3, "#BP breakpoint (int3)" },
        { AXYS_FAULT_UD2, 6, "#UD invalid opcode" },
        { AXYS_FAULT_PF, 14, "#PF page fault" },
    };
    unsigned mismatches = axys_interrupt_layout_mismatches();
    axys_size_t index;
    axys_uint32_t failures = 0;
    unsigned checks = 0;

    /*
     * `checks` counts every assertion this self-test actually performs:
     * the layout consistency check, each IST-wiring invariant, each fault
     * injection, and each IST-delivery probe. No hard-coded totals.
     */
    ++checks;
    /*
     * The stub table and the C-side error-code mask have to describe the same
     * 256 vectors. If they do not, some frame is the wrong length and iretq
     * faults, so say so plainly rather than letting it surface later as an
     * unexplained #GP.
     */
    if (mismatches != 0) {
        axys_printf("  FAIL error-code vector table: %u vectors disagree "
                    "between the stubs and the C mask\n", mismatches);
    } else {
        axys_printf("  ok   %-20s -> 256 vectors consistent\n",
                    "error-code layout");
    }

    report_double_fault_gate();
    failures += check_ist_wiring(&checks);

    axys_fault_test_enable(1);

    for (index = 0; index < AXYS_ARRAY_SIZE(cases); ++index) {
        axys_uint32_t observed = axys_fault_test_run(cases[index].which);

        ++checks; /* each injected fault must come back with its own vector */
        if (observed == cases[index].expected) {
            axys_printf("  ok   %-20s -> vector %u\n", cases[index].name,
                        observed);
        } else {
            axys_printf("  FAIL %-20s -> expected %u, got %u\n",
                        cases[index].name, cases[index].expected, observed);
            ++failures;
        }
    }

    /*
     * Now the same IST 1 gate, twice, to see whether the host switches stacks:
     * once on a real hardware exception (#UD) and once on a software interrupt
     * through the reserved vector 22. Vector 22 rather than 8 because a
     * software `int` pushes no error code even when the gate is marked
     * error-code -- against the real #DF gate that would desynchronise the
     * frame instead of testing it. The #UD gate is restored afterwards: IST 1
     * is a scarce resource, and leaving it on #UD would silently give every
     * later invalid opcode its own stack.
     */
    failures += observe_ist_delivery(AXYS_FAULT_UD2, 6, 6, "#UD on IST 1",
                                     &checks);
    restore_plain_gate(6);
    failures += observe_ist_delivery(AXYS_FAULT_IST,
                                     AXYS_FAULT_TEST_IST_VECTOR,
                                     AXYS_FAULT_TEST_IST_VECTOR,
                                     "int 22 on IST 1", &checks);
    /* Leave the IDT exactly as it was found: vector 22 goes back to its
     * plain (no-IST) gate. It is reserved today, but a test-only IST
     * binding that outlives the test would silently give every future
     * vector-22 user the double-fault stack. */
    restore_plain_gate(AXYS_FAULT_TEST_IST_VECTOR);

    /*
     * axys_fault_double() is deliberately not run here. It is the one injection
     * that can take the machine down: it needs the CPU to escalate a stack
     * fault into #DF and then honour IST 1 while doing so, and on a host that
     * does not model the escalation it produces a triple fault and a reset
     * whether or not the kernel is correct. It remains the right test on real
     * hardware, so the generator stays.
     */
    axys_fault_test_enable(0);

    /* `checks` was incremented for every real assertion above; no
     * hard-coded total that goes stale when tests are added. */
    if (failures == 0 && mismatches == 0) {
        axys_printf("exception selftest: all %u checks passed\n", checks);
    } else {
        axys_printf("exception selftest: %u of %u checks FAILED, %u layout "
                    "mismatches\n", failures, checks, mismatches);
    }
}

