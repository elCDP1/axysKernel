#include "axys/cpu.h"
#include "axys/interrupts.h"
#include "axys/io.h"
#include "axys/pit.h"
#include "axys/random.h"
#include "axys/sched.h"

/* Channel 0 of the PIT. Each channel has a data port and shares one control
 * port. */
#define PIT_CH0_DATA 0x40
#define PIT_COMMAND 0x43

/*
 * Command byte for channel 0, square wave mode 3, lobyte/hibyte access,
 * binary counting.
 *
 *   bits 7-6  channel select   00 = channel 0
 *   bits 5-4  access mode     11 = lobyte/hibyte
 *   bits 3-1  mode            011 = square wave
 *   bit  0    bcd/binary      0 = binary
 */
#define PIT_CMD_CH0_MODE3 0x36

static volatile axys_uint64_t pit_ticks;
static axys_uint32_t pit_hz;

void axys_pit_init(axys_uint32_t hz)
{
    axys_uint32_t divisor;

    if (hz == 0) {
        hz = AXYS_PIT_DEFAULT_HZ;
    }

    /*
     * 16 bits of divisor means the slowest achievable rate is
     * 1193182/65536 = 18.2 Hz. Asking for anything slower cannot be honoured,
     * so clamp rather than silently wrap the divisor around to something fast
     * and unexpected.
     */
    if (hz < 19) {
        hz = 19;
    }

    divisor = AXYS_PIT_INPUT_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 65536) {
        divisor = 65536;
    }

    /*
     * Report the rate actually achieved rather than the requested one. The
     * divisor is truncated, so 100 Hz becomes 1193182/11931 = 100.02 Hz;
     * anything converting ticks to time has to use this value, not the request.
     */
    pit_hz = AXYS_PIT_INPUT_HZ / divisor;

    /* Low byte then high byte: see the access mode in PIT_CMD_CH0_MODE3. */
    axys_outb(PIT_COMMAND, PIT_CMD_CH0_MODE3);
    axys_outb(PIT_CH0_DATA, (axys_uint8_t)(divisor & 0xffu));
    axys_outb(PIT_CH0_DATA, (axys_uint8_t)((divisor >> 8) & 0xffu));

    /*
     * Unmask only IRQ0. Every other line stays masked: an unmasked line whose
     * driver does not exist yet would vector straight into a null handler, and
     * the master cascade (IRQ2) in particular must stay masked or a spurious
     * master interrupt storms the CPU.
     */
    axys_pic_unmask(AXYS_PIT_IRQ);
}

static void pit_irq_handler(struct axys_interrupt_frame *frame)
{
    (void)frame;
    ++pit_ticks;
    axys_random_add_timing();
    axys_sched_tick();
}

void axys_pit_start(void)
{
    pit_ticks = 0;
    axys_irq_register(AXYS_PIT_IRQ, pit_irq_handler);
}

axys_uint64_t axys_pit_ticks(void)
{
    /*
     * A 64-bit read is not atomic on a 32-bit machine, and this is written by
     * an interrupt handler. The value is only ever used for elapsed-time
     * estimates, where a torn read showing a slightly stale count is harmless,
     * so no interrupt masking is done here -- it would be a poor trade to
     * close interrupts just to make a monotonic counter exact.
     */
    return pit_ticks;
}

axys_uint32_t axys_pit_hz(void)
{
    return pit_hz;
}

axys_uint64_t axys_pit_millis(void)
{
    axys_uint64_t whole_ticks;
    axys_uint64_t remainder;

    if (pit_hz == 0) {
        return 0;
    }

    /* Avoid overflowing pit_ticks * 1000 when the 64-bit tick counter has
     * been running for a long time. */
    whole_ticks = pit_ticks / pit_hz;
    remainder = pit_ticks % pit_hz;
    return whole_ticks * 1000u + (remainder * 1000u) / pit_hz;
}

void axys_pit_sleep_ms(axys_uint64_t ms)
{
    axys_uint64_t target;

    if (pit_hz == 0) {
        return;
    }
    target = axys_pit_millis() + ms;

    /*
     * Compare with wrapping arithmetic rather than !=, so a tick that arrives
     * while we are polling does not make the equality test miss its window and
     * hang forever. Yields between polls so the tick interrupt is actually
     * delivered instead of being delayed by a tight non-interruptible loop.
     */
    while ((axys_int64_t)(target - axys_pit_millis()) > 0) {
        axys_cpu_relax();
    }
}
