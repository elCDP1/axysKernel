#ifndef AXYS_SERIAL_H
#define AXYS_SERIAL_H

#include "axys/types.h"

/*
 * COM1 output. axys_serial_putc and axys_serial_write are the only paths the
 * console uses, so they have to be safe to call from the panic and exception
 * reporters -- which are themselves called when something has already gone
 * badly wrong. That is why the transmit-ready wait is bounded rather than
 * infinite: an absent or wedged UART must degrade to dropped characters, not
 * to a hang that hides the very diagnostics being printed.
 */
void axys_serial_init(void);
void axys_serial_putc(char character);
void axys_serial_write(const char *string, axys_size_t length);

/* Spins for the transmit holding register to drain, within a finite budget.
 * Returns non-zero if the UART is ready, 0 on timeout. */
int axys_serial_wait_ready(void);

#endif
