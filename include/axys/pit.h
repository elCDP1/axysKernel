#ifndef AXYS_PIT_H
#define AXYS_PIT_H

#include "axys/types.h"

/*
 * 8253/8254 Programmable Interval Timer, channel 0, driving IRQ0.
 *
 * The PIT is the reason IRQ0 shows up at all on a PC: it is wired to the
 * master's line 0 and is what turns "interrupts exist" into "interrupts
 * happen", so it is the first thing worth having working.
 *
 * Channel 0 is the tick channel. Counting from a 16-bit divisor, the input
 * clock is nominally 1.193182 MHz, so the tick rate is
 *
 *     rate = 1193182 / divisor
 *
 * and the largest divisor (65536, expressed as 0) gives the slowest tick,
 * about 18.2 Hz.
 */

#define AXYS_PIT_INPUT_HZ 1193182u
#define AXYS_PIT_IRQ 0u
#define AXYS_PIT_DEFAULT_HZ 100u

/* Program channel 0 to a square wave at hz. The divisor is computed by
 * integer truncation (1193182 / hz); axys_pit_hz() reports the rate that
 * divisor actually produces, which may differ slightly from the request. */
void axys_pit_init(axys_uint32_t hz);

/* Register the IRQ0 handler and zero the tick count. Call before
 * axys_pit_init, or at least before unmasking IRQ0, so no tick can arrive with
 * no handler installed. */
void axys_pit_start(void);

/* Current tick count, as incremented by the IRQ0 handler. */
axys_uint64_t axys_pit_ticks(void);

/* The rate the timer was actually programmed to, in Hz. This is the requested
 * rate after truncating to a whole divisor, and is the number to use when
 * converting ticks to time -- assuming the nominal input clock, which is not
 * guaranteed on modern hardware. */
axys_uint32_t axys_pit_hz(void);

/* Rough milliseconds since the timer started. Derived from the tick count, so
 * it inherits whatever error the input clock has. */
axys_uint64_t axys_pit_millis(void);

/* Busy-wait for approximately ms milliseconds, yielding between polls. */
void axys_pit_sleep_ms(axys_uint64_t ms);

#endif
