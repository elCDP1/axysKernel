#ifndef AXYS_CSI_H
#define AXYS_CSI_H

#include "axys/types.h"

/*
 * Minimal ANSI CSI decoder (ESC [ params final), shared by the VGA console
 * and unit-tested on the host. Only what a kernel console needs:
 *
 *   J  erase display: 0/empty cursor-to-end, 1 start-to-cursor, 2 all
 *   H  cursor motion: home, or 1-based row;col (clamped by the caller)
 *   f  same as H
 *   K  erase line: 0/empty cursor-to-end, 1 start-to-cursor, 2 whole line
 *   m  SGR attributes (0 reset, 1 bold, 30-37 fg, 40-47 bg; rest ignored)
 *   ?25 l/h  hide/show the cursor
 *   ?1049 h/l  alternate screen on/off
 *
 * Anything else well-formed completes with action NONE (swallow, never
 * print); malformed input aborts to ground. Parameters saturate instead of
 * overflowing; at most 4 are kept. Feed one byte at a time:
 *
 *   AXYS_CSI_PRINT   ordinary byte: the caller prints it
 *   AXYS_CSI_SWALLOW byte belongs to an unfinished sequence: print nothing
 *   AXYS_CSI_DO      a sequence just completed: `action`/params valid
 *
 * Missing or zero parameters arrive as 0; the caller applies its defaults
 * (tests assert the raw 0 so both layers stay honest).
 */

enum axys_csi_result {
    AXYS_CSI_PRINT,
    AXYS_CSI_SWALLOW,
    AXYS_CSI_DO
};

enum axys_csi_action {
    AXYS_CSI_NONE,
    AXYS_CSI_ERASE_DISPLAY,
    AXYS_CSI_CURSOR_TO,
    AXYS_CSI_ERASE_LINE,
    AXYS_CSI_SGR,
    AXYS_CSI_CURSOR_HIDE,
    AXYS_CSI_CURSOR_SHOW,
    AXYS_CSI_ALT_ON,
    AXYS_CSI_ALT_OFF
};

#define AXYS_CSI_PARAMS 4u
#define AXYS_CSI_SATURATE 9999u

struct axys_csi {
    int state; /* 0 ground, 1 after ESC, 2 inside CSI */
    axys_uint32_t param[AXYS_CSI_PARAMS];
    axys_uint32_t started; /* parameters begun so far */
    int digit;             /* current parameter has digits */
    int drop;              /* beyond capacity: ignore until final */
    int question;          /* private '?', only as the very first char */
    int action;            /* enum axys_csi_action, valid after DO */
    axys_uint32_t arg0;
    axys_uint32_t arg1;
};

static void axys_csi_reset(struct axys_csi *st)
{
    axys_uint32_t i;

    st->state = 0;
    for (i = 0; i < AXYS_CSI_PARAMS; ++i) {
        st->param[i] = 0;
    }
    st->started = 0;
    st->digit = 0;
    st->drop = 0;
    st->question = 0;
    st->action = AXYS_CSI_NONE;
    st->arg0 = 0;
    st->arg1 = 0;
}

/* Decoded parameter i, or 0 when missing. */
static axys_uint32_t axys_csi_raw(const struct axys_csi *st, axys_uint32_t index)
{
    axys_uint32_t total = st->started + (st->digit ? 1u : 0u);

    if (index >= total || index >= AXYS_CSI_PARAMS) {
        return 0;
    }
    return st->param[index];
}

/* How many parameters the completed sequence carries (missing slots count:
 * `ESC[;5H` and `ESC[m` both count what the caller must iterate). */
static axys_uint32_t axys_csi_count(const struct axys_csi *st)
{
    axys_uint32_t total = st->started + (st->digit ? 1u : 0u);

    if (total == 0u) {
        total = 1u; /* bare final byte means one (empty) parameter */
    }
    if (total > AXYS_CSI_PARAMS) {
        total = AXYS_CSI_PARAMS;
    }
    return total;
}

static int axys_csi_feed(struct axys_csi *st, char c)
{
    if (st->state == 2) {
        if (c == '?' && !st->question && !st->digit && st->started == 0u && !st->drop) {
            /* Private marker, only as the very first character. */
            st->question = 1;
            return AXYS_CSI_SWALLOW;
        }
        if (c >= '0' && c <= '9') {
            if (!st->drop && st->started < AXYS_CSI_PARAMS) {
                axys_uint32_t digit = (axys_uint32_t)(c - '0');

                if (st->param[st->started] <= (AXYS_CSI_SATURATE - digit) / 10u) {
                    st->param[st->started] = st->param[st->started] * 10u + digit;
                } else {
                    st->param[st->started] = AXYS_CSI_SATURATE;
                }
                st->digit = 1;
            }
            return AXYS_CSI_SWALLOW;
        }
        if (c == ';') {
            if (st->started + 1u < AXYS_CSI_PARAMS) {
                ++st->started;
            } else {
                st->drop = 1;
            }
            st->digit = 0;
            return AXYS_CSI_SWALLOW;
        }
        if (c >= '@' && c <= '~') {
            st->action = AXYS_CSI_NONE;
            st->arg0 = axys_csi_raw(st, 0);
            st->arg1 = axys_csi_raw(st, 1);
            if (!st->question) {
                if (c == 'J') {
                    st->action = AXYS_CSI_ERASE_DISPLAY;
                } else if (c == 'H' || c == 'f') {
                    st->action = AXYS_CSI_CURSOR_TO;
                } else if (c == 'K') {
                    st->action = AXYS_CSI_ERASE_LINE;
                } else if (c == 'm') {
                    st->action = AXYS_CSI_SGR;
                }
            } else if ((c == 'l' || c == 'h') && !st->drop &&
                       st->started == 0u && st->digit) {
                /* Single numeric private parameter only. */
                if (st->param[0] == 25u) {
                    st->action = c == 'l' ? AXYS_CSI_CURSOR_HIDE : AXYS_CSI_CURSOR_SHOW;
                } else if (st->param[0] == 1049u) {
                    st->action = c == 'h' ? AXYS_CSI_ALT_ON : AXYS_CSI_ALT_OFF;
                }
            }
            /* Any other private sequence swallows whole: printing its tail
             * would litter program output with digits. */
            st->state = 0;
            return AXYS_CSI_DO;
        }
        /* Anything else: abort the sequence. */
        st->state = 0;
        return AXYS_CSI_SWALLOW;
    }
    if (st->state == 1) {
        if (c == '[') {
            axys_uint32_t i;

            /* Fresh parameter slots: without this, `ESC[;5H` would inherit
             * slot 0 from whatever sequence ran before it. */
            for (i = 0; i < AXYS_CSI_PARAMS; ++i) {
                st->param[i] = 0;
            }
            st->started = 0;
            st->digit = 0;
            st->drop = 0;
            st->question = 0;
            st->action = AXYS_CSI_NONE;
            st->state = 2;
        } else if (c == 0x1b) {
            st->state = 1; /* ESC ESC: stay armed */
        } else {
            st->state = 0; /* stray ESC plus follower: drop both */
        }
        return AXYS_CSI_SWALLOW;
    }
    if (c == 0x1b) {
        st->state = 1;
        return AXYS_CSI_SWALLOW;
    }
    return AXYS_CSI_PRINT;
}

#endif
