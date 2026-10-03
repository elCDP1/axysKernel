#include "axys/cpu.h"
#include "axys/exceptions.h"
#include "axys/idt.h"
#include "axys/interrupts.h"
#include "axys/io.h"
#include "axys/panic.h"
#include "axys/printf.h"
#include "axys/process.h"
#include "axys/sched.h"
#include "axys/string.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA 0xa1
#define PIC1_VECTOR_OFFSET 0x20
#define PIC2_VECTOR_OFFSET 0x28
#define PIC_EOI 0x20
#define PIC1_CASCADE_MASK 0x04 /* IRQ2 is wired to the slave's cascade  */
#define PIC2_CASCADE_MASK 0x02
#define PIC_CASCADE_IRQ 2

static axys_irq_handler_t irq_handlers[AXYS_IDT_IRQ_COUNT];
static axys_uint64_t irq_counts[AXYS_IDT_IRQ_COUNT];
static axys_uint64_t irq_total;

unsigned axys_interrupt_layout_mismatches(void)
{
    /*
     * Cross-check the assembly stub table against the C-side mask.
     *
     * "Does this vector push an error code" is stated twice -- once as
     * ISR_ERROR/ISR_NOERR in the .S and once as AXYS_IRQ_ERROR_VECTORS in the
     * header -- and nothing but this function connects them. A vector
     * classified differently in the two places produces a frame one word short
     * or one word long, and the visible result is a #GP raised at the iretq
     * with a selector made of the saved RFLAGS. That is a miserable thing to
     * debug from a triple fault, so it is checked explicitly at boot.
     */
    unsigned vector;
    unsigned mismatches = 0;

    for (vector = 0; vector < 256; ++vector) {
        int from_asm = axys_error_vector_table[vector] != 0;
        int from_c = vector < 64 &&
                    (AXYS_IRQ_ERROR_VECTORS & (1ull << vector)) != 0;

        if (from_asm != from_c) {
            ++mismatches;
            axys_printf("  vector %u: stub-table says %d, C mask says %d\n",
                        vector, from_asm, from_c);
        }
    }
    return mismatches;
}

void axys_pic_remap(void)
{
    /*
     * Mask both PICs first. GRUB leaves a timer IRQ latched, and an unmasked
     * line while the vector layout is being changed would deliver vector 0x20
     * straight into an IDT that is still being written.
     */
    axys_pic_mask_all();

    /* ICW1: 0x11 = expect ICW4, initialise. */
    axys_outb(PIC1_COMMAND, 0x11);
    axys_io_wait();
    axys_outb(PIC2_COMMAND, 0x11);
    axys_io_wait();

    /* ICW2: vector offsets, chosen to leave 0..31 for CPU exceptions. */
    axys_outb(PIC1_DATA, PIC1_VECTOR_OFFSET);
    axys_io_wait();
    axys_outb(PIC2_DATA, PIC2_VECTOR_OFFSET);
    axys_io_wait();

    /* ICW3: cascade wiring (master IRQ2 <- slave). */
    axys_outb(PIC1_DATA, PIC1_CASCADE_MASK);
    axys_io_wait();
    axys_outb(PIC2_DATA, PIC2_CASCADE_MASK);
    axys_io_wait();

    /* ICW4: 0x01 = 8086 mode, not fully nested. */
    axys_outb(PIC1_COMMAND, 0x01);
    axys_io_wait();
    axys_outb(PIC2_COMMAND, 0x01);
    axys_io_wait();

    axys_pic_mask_all();
    axys_memset(irq_handlers, 0, sizeof(irq_handlers));
    axys_memset(irq_counts, 0, sizeof(irq_counts));
    irq_total = 0;
}

/*
 * Shadow copies of the two interrupt mask registers.
 *
 * axys_pic_mask/unmask deliberately do NOT read the mask back with inb to do a
 * read-modify-write. The 8259's data port is not reliably readable across
 * emulators and firmware handoff states -- on this setup it reads back as 0
 * regardless of what was written -- so a RMW would compute the new mask from
 * garbage and could unmask every line at once. Tracking the mask in software
 * and always writing the whole byte makes the result independent of whether
 * the read side works at all.
 */
static axys_uint8_t pic1_mask = 0xff;
static axys_uint8_t pic2_mask = 0xff;

static void axys_pic_write_masks(void)
{
    axys_outb(PIC1_DATA, pic1_mask);
    axys_io_wait();
    axys_outb(PIC2_DATA, pic2_mask);
    axys_io_wait();
}

void axys_pic_unmask(axys_uint8_t irq)
{
    if (irq >= AXYS_IDT_IRQ_COUNT) {
        return;
    }

    /*
     * Refuse to unmask the cascade. IRQ2 is the master's input from the slave
     * PIC, not a device: it is asserted whenever the slave has *any* pending
     * interrupt, including one that resolves to nothing. Letting the master
     * deliver it produces a storm of meaningless vectors and, because a real
     * slave interrupt still needs its own EOI, wedges both chips. The comment
     * here used to state this rule while the code happily cleared the bit, so
     * it is now enforced rather than just documented.
     */
    if (irq == PIC_CASCADE_IRQ) {
        return;
    }

    /*
     * IRQ8..15 live on the slave, whose lines are offset by 8 within the byte.
     */
    if (irq < 8) {
        pic1_mask = (axys_uint8_t)(pic1_mask & (axys_uint8_t)~(1u << irq));
    } else {
        pic2_mask = (axys_uint8_t)(pic2_mask & (axys_uint8_t)~(1u << (irq - 8)));
        pic1_mask = (axys_uint8_t)(pic1_mask & (axys_uint8_t)~PIC1_CASCADE_MASK);
    }
    axys_pic_write_masks();
}

void axys_pic_mask(axys_uint8_t irq)
{
    if (irq >= AXYS_IDT_IRQ_COUNT) {
        return;
    }
    if (irq < 8) {
        pic1_mask = (axys_uint8_t)(pic1_mask | (1u << irq));
    } else {
        pic2_mask = (axys_uint8_t)(pic2_mask | (1u << (irq - 8)));
        /* Cascade bookkeeping: IRQ2 on the master only needs to stay open
         * while at least one slave line is unmasked. Once the last slave
         * line is masked again, close it so a spurious slave assertion
         * cannot deliver a meaningless cascade vector. */
        if (pic2_mask == 0xff) {
            pic1_mask = (axys_uint8_t)(pic1_mask | PIC1_CASCADE_MASK);
        }
    }
    axys_pic_write_masks();
}

void axys_pic_mask_all(void)
{
    pic1_mask = 0xff;
    pic2_mask = 0xff;
    axys_pic_write_masks();
}

void axys_pic_unmask_all_but_cascade(void)
{
    /* Historical name, precise semantics: every master line except the
     * IRQ2 cascade is opened, and every slave line is opened in the
     * software shadow -- but the cascade itself stays masked, so slave
     * interrupts are recorded yet cannot actually reach the CPU. Use
     * axys_pic_unmask() per-IRQ for a usable configuration; this call
     * exists mainly as a known-safe intermediate state. */
    pic1_mask = PIC1_CASCADE_MASK;
    pic2_mask = 0x00;
    axys_pic_write_masks();
}

void axys_irq_register(axys_uint8_t irq, axys_irq_handler_t handler)
{
    if (irq >= AXYS_IDT_IRQ_COUNT) {
        return;
    }
    irq_handlers[irq] = handler;
}

axys_uint64_t axys_irq_count(axys_uint8_t irq)
{
    if (irq >= AXYS_IDT_IRQ_COUNT) {
        return 0;
    }
    return irq_counts[irq];
}

axys_uint64_t axys_irq_total(void)
{
    return irq_total;
}

void axys_irq_reset_counters(void)
{
    axys_memset(irq_counts, 0, sizeof(irq_counts));
    irq_total = 0;
}

/*
 * Acknowledge an interrupt.
 *
 * A line on the slave is acknowledged on the slave *and* on the master,
 * because the master latches the cascade. Getting this wrong leaves the
 * interrupt stuck in service and the line permanently blocked.
 */
static void pic_send_eoi(axys_uint8_t vector)
{
    if (vector >= PIC2_VECTOR_OFFSET && vector < PIC2_VECTOR_OFFSET + 8) {
        axys_outb(PIC2_COMMAND, PIC_EOI);
        axys_io_wait();
    }
    axys_outb(PIC1_COMMAND, PIC_EOI);
    axys_io_wait();
}

/*
 * Spurious-interrupt detection on the two lines that can generate them.
 *
 * An 8259 can raise its highest-priority line (IRQ7 on the master, IRQ15 on
 * the slave) without a real device behind it -- typically an edge glitch or a
 * device whose line was masked between the assertion and the ISR read. The
 * architecture's test is to read the ISR right after entry: if the
 * corresponding ISR bit is clear, no line is actually in service and the
 * interrupt was spurious. Such interrupts must not be dispatched to a
 * handler, but the master still needs its EOI (the cascade latched one).
 */
#define PIC_READ_ISR 0x0bu

static int pic_irq_is_spurious(axys_uint8_t irq)
{
    axys_uint8_t isr;

    if (irq != 7 && irq != 15) {
        return 0; /* only the top-priority lines of each chip can spur */
    }
    if (irq == 7) {
        /* OCW3 is written to, and the ISR read back from, the *command*
         * port -- 0x20/0xa0, never the data port (0x21/0xa1 is the IMR, an
         * unrelated register). Reading the data port here silently read the
         * mask instead of the in-service state, so the check never worked:
         * it would misclassify real interrupts as spurious. */
        axys_outb(PIC1_COMMAND, PIC_READ_ISR);
        axys_io_wait();
        isr = axys_inb(PIC1_COMMAND);
        return (isr & 0x80u) == 0;
    }
    axys_outb(PIC2_COMMAND, PIC_READ_ISR);
    axys_io_wait();
    isr = axys_inb(PIC2_COMMAND);
    if ((isr & 0x80u) != 0) {
        return 0; /* slave really has IRQ15 in service */
    }
    /* Slave ISR says no: check whether the master merely latched a spurious
     * cascade (its own IRQ7 bit set by the cascade pulse). If the master's
     * cascade bit is also clear, this is a genuine spurious delivery. */
    axys_outb(PIC1_COMMAND, PIC_READ_ISR);
    axys_io_wait();
    isr = axys_inb(PIC1_COMMAND);
    return (isr & PIC1_CASCADE_MASK) == 0;
}

static void irq_dispatch(struct axys_interrupt_frame *frame)
{
    axys_uint8_t vector = (axys_uint8_t)frame->vector;
    axys_uint8_t irq;
    axys_irq_handler_t handler;

    if (vector < AXYS_IDT_IRQ_VECTOR_BASE) {
        return;
    }
    irq = (axys_uint8_t)(vector - AXYS_IDT_IRQ_VECTOR_BASE);
    if (irq >= AXYS_IDT_IRQ_COUNT) {
        return;
    }

    ++irq_counts[irq];
    ++irq_total;

    if (!pic_irq_is_spurious(irq)) {
        handler = irq_handlers[irq];
        if (handler != AXYS_NULL) {
            handler(frame);
        }
    }

    /* EOI goes out even with no handler (and for spurious deliveries):
     * otherwise the line stays blocked forever after the first interrupt. */
    pic_send_eoi(vector);

    /* Task switches happen only after the EOI, so the PIC is never left
     * waiting on an interrupt that a switched-out task still "owns". */
    axys_sched_preempt();
    if ((frame->cs & 3u) == 3u) {
        axys_process_check_kill(); /* interrupted user code: safe point to die */
    }
}

/*
 * Vectors outside 0..47 have no meaning in this kernel yet: nothing owns
 * them, so any delivery is a bug (a stray INT n from corrupted code, a
 * misconfigured device about to get an APIC vector, etc.). Report loudly
 * instead of returning silently -- a silent drop hides exactly the kind of
 * state corruption this kernel panics on everywhere else. No PIC EOI is
 * sent: these vectors did not come from the PIC.
 */
static void unexpected_vector_dispatch(struct axys_interrupt_frame *frame)
{
    char message[96];

    axys_snprintf(message, sizeof(message),
                  "unexpected interrupt vector %u (valid ranges:"
                  " 0-31 exceptions, 32-47 PIC IRQs)", (unsigned)frame->vector);
    axys_panic(message); /* does not return */
}

void axys_isr_handler(struct axys_interrupt_frame *frame)
{
    axys_uint32_t vector = (axys_uint32_t)frame->vector;

    if (vector >= AXYS_IDT_IRQ_VECTOR_END) {
        unexpected_vector_dispatch(frame);
        return;
    }
    if (vector >= AXYS_IDT_IRQ_VECTOR_BASE) {
        irq_dispatch(frame);
        return;
    }

    axys_exception_entry(frame);
}
