#ifndef AXYS_INPUT_H
#define AXYS_INPUT_H

#include "axys/types.h"

/*
 * Console input: PS/2 keyboard (i8042), USB HID keyboards (xHCI polling) and
 * serial RX feed one ring buffer.
 *
 * The PS/2 keyboard is decoded from scancode set 1 (the controller's
 * translated default) for a US layout: shift, caps lock and ctrl work, and the
 * cursor/navigation keys are delivered as VT100 escape sequences. USB HID
 * keyboards arrive as boot-protocol reports through kernel/usb_hid.c with the
 * same US layout semantics. Serial input is delivered as-is except CR -> LF
 * and DEL -> backspace.
 */

/* Set up the controller, register IRQ 1 and IRQ 4 handlers and unmask them.
 * Needs the PIC/IDT and the scheduler. Returns 0, or -1 when no i8042 exists
 * (serial input still works in that case). */
int axys_input_init(void);

/* Next character, or -1 when nothing is buffered. Safe from any context. */
int axys_input_getc_nonblock(void);

/* Next character, blocking the calling task until one arrives. */
int axys_input_getc(void);

/* Read a line with echo and backspace editing into `buffer` (NUL-terminated,
 * newline not stored). Returns the length. Blocks. */
axys_size_t axys_input_readline(char *buffer, axys_size_t size);

axys_uint64_t axys_input_dropped(void); /* characters lost to a full buffer */

/* Push one decoded character from a non-IRQ producer (USB HID polling task).
 * Wakes tasks blocked in axys_input_getc(). Safe from any context. */
void axys_input_push_char(char c);

#endif
