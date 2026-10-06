/* USB HID boot-keyboard decoder tests: press/release, shift pairs, caps lock,
 * ctrl chording, rollover and navigation escape sequences. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "axys/usb_hid.h"

static void press(struct axys_hid_kbd *kbd, unsigned char mod, unsigned char key, char *out,
                  int *n)
{
    unsigned char report[8] = {mod, 0, key, 0, 0, 0, 0, 0};

    *n = axys_hid_kbd_report(kbd, report, out, (unsigned)16);
}

static void release_all(struct axys_hid_kbd *kbd)
{
    unsigned char report[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    char out[16];

    axys_hid_kbd_report(kbd, report, out, (unsigned)16);
}

int main(void)
{
    struct axys_hid_kbd kbd;
    char out[16];
    int n;

    axys_hid_kbd_init(&kbd);

    /* Simple press: 'a' (0x04), then held (no repeat), then release. */
    press(&kbd, 0, 0x04, out, &n);
    assert(n == 1 && out[0] == 'a');
    press(&kbd, 0, 0x04, out, &n);
    assert(n == 0);
    release_all(&kbd);

    /* Shift pairs: 'A' and '!'. */
    press(&kbd, 0x02, 0x04, out, &n);
    assert(n == 1 && out[0] == 'A');
    release_all(&kbd);
    press(&kbd, 0x20, 0x1e, out, &n);
    assert(n == 1 && out[0] == '!');
    release_all(&kbd);

    /* Enter, backspace, tab, space. */
    press(&kbd, 0, 0x28, out, &n);
    assert(n == 1 && out[0] == '\n');
    release_all(&kbd);
    press(&kbd, 0, 0x2a, out, &n);
    assert(n == 1 && out[0] == '\b');
    release_all(&kbd);
    press(&kbd, 0, 0x2c, out, &n);
    assert(n == 1 && out[0] == ' ');
    release_all(&kbd);

    /* Caps lock toggles letters only. */
    press(&kbd, 0, 0x39, out, &n);
    assert(n == 0);
    release_all(&kbd);
    press(&kbd, 0, 0x04, out, &n);
    assert(n == 1 && out[0] == 'A');
    release_all(&kbd);
    press(&kbd, 0, 0x1e, out, &n);
    assert(n == 1 && out[0] == '1');
    release_all(&kbd);
    press(&kbd, 0, 0x39, out, &n);
    release_all(&kbd);
    press(&kbd, 0, 0x04, out, &n);
    assert(n == 1 && out[0] == 'a');
    release_all(&kbd);

    /* Ctrl chords to control characters (Ctrl-C). */
    press(&kbd, 0x01, 0x06, out, &n);
    assert(n == 1 && out[0] == 0x03);
    release_all(&kbd);

    /* Arrows come out as VT100. */
    press(&kbd, 0, 0x52, out, &n);
    assert(n == 3 && memcmp(out, "\033[A", 3) == 0);
    release_all(&kbd);
    press(&kbd, 0, 0x4c, out, &n);
    assert(n == 4 && memcmp(out, "\033[3~", 4) == 0);
    release_all(&kbd);

    /* ErrorRollOver is ignored, unmapped usages produce nothing. */
    {
        unsigned char roll[8] = {0, 0, 1, 1, 1, 1, 1, 1};

        assert(axys_hid_kbd_report(&kbd, roll, out, (unsigned)16) == 0);
    }
    press(&kbd, 0, 0x3a, out, &n); /* F1: unmapped */
    assert(n == 0);
    release_all(&kbd);

    /* Two fresh keys in one report both emit. */
    {
        unsigned char report[8] = {0, 0, 0x04, 0x05, 0, 0, 0, 0};

        assert(axys_hid_kbd_report(&kbd, report, out, (unsigned)16) == 2);
        assert(out[0] == 'a' && out[1] == 'b');
        release_all(&kbd);
    }

    /* Hostile reports must never write past the caller's buffer: six keys that
     * expand to 4-byte VT100 sequences in a single 8-byte report. */
    {
        unsigned char six[8] = {0, 0, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c, 0x4c};
        char guarded[6];
        char full[24];

        release_all(&kbd);
        memset(guarded, 'Z', sizeof(guarded));
        n = axys_hid_kbd_report(&kbd, six, guarded, (unsigned)sizeof(guarded));
        assert(n == 4); /* a sequence is emitted whole or not at all */
        assert(guarded[3] == '~');
        assert(guarded[4] == 'Z' && guarded[5] == 'Z');
        release_all(&kbd);
        n = axys_hid_kbd_report(&kbd, six, full, (unsigned)sizeof(full));
        assert(n == 24); /* worst case fits exactly */
        release_all(&kbd);
    }

    /* NULL arguments never crash. */
    assert(axys_hid_kbd_report(NULL, NULL, NULL, 0) == 0);
    assert(axys_hid_kbd_report(&kbd, NULL, out, 16) == 0);

    printf("test_usb: ok\n");
    return 0;
}
