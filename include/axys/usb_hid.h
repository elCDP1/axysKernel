#ifndef AXYS_USB_HID_H
#define AXYS_USB_HID_H

#include "axys/types.h"

/*
 * USB HID boot-protocol keyboard decoder (report = 8 bytes: modifiers,
 * reserved, 6 keycodes). US layout with the same semantics as the PS/2 path:
 * shift pairs, caps lock over letters, ctrl chording, arrows/home/end/delete
 * as VT100 escape sequences. Pure logic, no hardware access.
 */

struct axys_hid_kbd {
    axys_uint8_t prev[6];
    int caps_lock;
};

void axys_hid_kbd_init(struct axys_hid_kbd *kbd);

/* Feed one 8-byte report. Emits up to `cap` characters into `out` and returns
 * how many; the caller sizes `out` for the worst case (6 keys x 4 bytes for
 * VT100 sequences = 24) and passes that size. A report that would not fit is
 * truncated at `cap` so a hostile device can never write past the buffer.
 * Releases produce no output; ErrorRollOver reports are ignored. */
int axys_hid_kbd_report(struct axys_hid_kbd *kbd, const axys_uint8_t *report, char *out,
                        unsigned cap);

#endif
