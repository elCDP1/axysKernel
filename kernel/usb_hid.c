#include "axys/usb_hid.h"

void axys_hid_kbd_init(struct axys_hid_kbd *kbd)
{
    for (unsigned i = 0; i < 6; ++i) {
        kbd->prev[i] = 0;
    }
    kbd->caps_lock = 0;
}

static int ctrl_down(axys_uint8_t modifiers)
{
    return ((modifiers & 0x01u) != 0) || ((modifiers & 0x10u) != 0);
}

/* HID usage ID (page 7) to ASCII, US layout. `shift` selects the shifted
 * symbol for the digit/symbol rows. Returns 0 for unmapped usages. */
static char usage_to_ascii(axys_uint8_t usage, int shift)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char digits[] = "1234567890";
    static const char shifted[] = "!@#$%^&*()";

    if (usage >= 0x04u && usage <= 0x1du) {
        return shift ? upper[usage - 0x04u] : lower[usage - 0x04u];
    }
    if (usage >= 0x1eu && usage <= 0x27u) {
        return shift ? shifted[usage - 0x1eu] : digits[usage - 0x1eu];
    }
    switch (usage) {
    case 0x28: return '\n'; /* Enter */
    case 0x29: return 0x1b; /* Escape */
    case 0x2a: return '\b'; /* Backspace */
    case 0x2b: return '\t'; /* Tab */
    case 0x2c: return ' ';  /* Space */
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

/* Navigation usages to VT100, mirroring the PS/2 path. Returns the sequence
 * length (0 when unmapped) and writes at most 4 bytes. */
static int usage_to_escape(axys_uint8_t usage, char *out)
{
    const char *seq = AXYS_NULL;

    switch (usage) {
    case 0x52: seq = "\033[A"; break; /* up */
    case 0x51: seq = "\033[B"; break; /* down */
    case 0x4f: seq = "\033[C"; break; /* right */
    case 0x50: seq = "\033[D"; break; /* left */
    case 0x4a: seq = "\033[H"; break; /* home */
    case 0x4d: seq = "\033[F"; break; /* end */
    case 0x4c: seq = "\033[3~"; break; /* delete */
    default: return 0;
    }
    {
        int n = 0;

        while (seq[n] != '\0') {
            out[n] = seq[n];
            ++n;
        }
        return n;
    }
}

static int was_down(const struct axys_hid_kbd *kbd, axys_uint8_t usage)
{
    for (unsigned i = 0; i < 6; ++i) {
        if (kbd->prev[i] == usage) {
            return 1;
        }
    }
    return 0;
}

int axys_hid_kbd_report(struct axys_hid_kbd *kbd, const axys_uint8_t *report, char *out)
{
    axys_uint8_t modifiers;
    int n = 0;

    if (kbd == AXYS_NULL || report == AXYS_NULL || out == AXYS_NULL) {
        return 0;
    }
    /* ErrorRollOver: more keys than fit; ignore the whole report. */
    {
        int rollover = 1;

        for (unsigned i = 2; i < 8; ++i) {
            if (report[i] != 0x01u) {
                rollover = 0;
                break;
            }
        }
        if (rollover) {
            goto save;
        }
    }
    modifiers = report[0];
    for (unsigned i = 2; i < 8; ++i) {
        axys_uint8_t usage = report[i];
        int shift;
        char c;

        if (usage == 0 || was_down(kbd, usage)) {
            continue; /* empty slot or held key: no repeat from the device */
        }
        if (usage == 0x39u) { /* CapsLock toggles on press */
            kbd->caps_lock = !kbd->caps_lock;
            continue;
        }
        {
            char esc[4];
            int m = usage_to_escape(usage, esc);

            if (m != 0) {
                for (int k = 0; k < m; ++k) {
                    out[n++] = esc[k];
                }
                continue;
            }
        }
        shift = ((modifiers & 0x02u) != 0) || ((modifiers & 0x20u) != 0);
        c = usage_to_ascii(usage, shift);
        if (c == 0) {
            continue;
        }
        if (kbd->caps_lock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c = (char)(c ^ 0x20);
        }
        if (ctrl_down(modifiers) &&
            ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c = (char)(c & 0x1f);
        }
        out[n++] = c;
    }
save:
    for (unsigned i = 0; i < 6; ++i) {
        kbd->prev[i] = report[i + 2];
    }
    return n;
}
