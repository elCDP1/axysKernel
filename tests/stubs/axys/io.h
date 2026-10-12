#ifndef AXYS_STUB_IO_H
#define AXYS_STUB_IO_H

#include "axys/types.h"

/* Privileged port I/O cannot run on the host: record CRTC cursor traffic,
 * drop everything else. Lets host tests drive kernel/vga.c against an
 * mmap()'d text buffer. State lives in the test (extern here) so both
 * translation units observe the same cursor. */
extern unsigned stub_crtc_cmd;
extern unsigned stub_cursor_addr;

static inline void axys_outb(axys_uint16_t port, axys_uint8_t value)
{
    if (port == 0x3d4u) {
        stub_crtc_cmd = value;
    } else if (port == 0x3d5u) {
        if (stub_crtc_cmd == 0x0fu) {
            stub_cursor_addr = (stub_cursor_addr & 0xff00u) | value;
        } else if (stub_crtc_cmd == 0x0eu) {
            stub_cursor_addr = (stub_cursor_addr & 0x00ffu) | ((unsigned)value << 8);
        }
    }
}

static inline axys_uint8_t axys_inb(axys_uint16_t port)
{
    (void)port;
    return 0;
}

#endif
