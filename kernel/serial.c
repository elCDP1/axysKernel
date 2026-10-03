#include "axys/cpu.h"
#include "axys/serial.h"
#include "axys/io.h"

#define COM1 0x3f8
#define UART_DATA 0
#define UART_INTERRUPT_ENABLE 1
#define UART_DIVISOR_LOW 0
#define UART_DIVISOR_HIGH 1
#define UART_FIFO_CONTROL 2
#define UART_LINE_CONTROL 3
#define UART_MODEM_CONTROL 4
#define UART_LINE_STATUS 5

#define UART_LINE_STATUS_THRE 0x20 /* transmit holding register empty */

/*
 * Bounded spin budget for "is the transmit holding register empty?".
 *
 * At the 115200 baud rate configured below, one character takes ~87 us, so
 * 200000 reads of a fast device is far more than any real drain time. The point
 * is not to be generous: it is to be finite. Every caller is a diagnostic path
 * that may be running on a machine with no UART at all, and an unbounded
 * `while (!(lsr & THRE))` there hangs the CPU inside the panic/exception
 * reporter, so the original fault is never printed and the machine simply
 * stops. On timeout the character is dropped and the caller carries on.
 */
#define UART_READY_SPINS 200000

static int serial_ready;

void axys_serial_init(void)
{
    axys_outb(COM1 + UART_INTERRUPT_ENABLE, 0x00);
    axys_outb(COM1 + UART_LINE_CONTROL, 0x80);
    axys_outb(COM1 + UART_DIVISOR_LOW, 0x01);
    axys_outb(COM1 + UART_DIVISOR_HIGH, 0x00);
    axys_outb(COM1 + UART_LINE_CONTROL, 0x03);
    axys_outb(COM1 + UART_FIFO_CONTROL, 0xc7);
    axys_outb(COM1 + UART_MODEM_CONTROL, 0x0b);
    serial_ready = 1;
}

int axys_serial_wait_ready(void)
{
    unsigned spins;

    for (spins = 0; spins < UART_READY_SPINS; ++spins) {
        if ((axys_inb(COM1 + UART_LINE_STATUS) & UART_LINE_STATUS_THRE) != 0) {
            return 1;
        }
        axys_cpu_relax();
    }
    return 0;
}

void axys_serial_putc(char character)
{
    if (!serial_ready) {
        return;
    }
    /* Drop the character rather than block forever if the UART never drains. */
    if (!axys_serial_wait_ready()) {
        return;
    }
    axys_outb(COM1 + UART_DATA, (axys_uint8_t)character);
}

void axys_serial_write(const char *string, axys_size_t length)
{
    for (axys_size_t index = 0; index < length; ++index) {
        axys_serial_putc(string[index]);
    }
}
