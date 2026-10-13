#ifndef AXYS_CODE_KEY_H
#define AXYS_CODE_KEY_H

#include "axys/types.h"

/*
 * Key decoder for /bin/code: raw console bytes become key events. The
 * kernel already delivers whole ESC sequences (arrows, Home/End, Delete
 * from PS/2 and USB HID); serial terminals add their own (Shift+arrows
 * as `ESC[1;2X`, Ctrl+Tab as `ESC[Z`). Feed one byte at a time:
 *
 *   1  one complete event in ev
 *   0  need more bytes
 *   2  ev holds a lone-ESC event AND the byte must be fed again in the
 *      now-ground decoder (ESC followed by a non-sequence byte: the ESC
 *      counts, the follower is not lost)
 *   3  ev holds a lead-alone CHAR AND the byte must be fed again (a broken
 *      multibyte run: the lead counts as one byte, the follower is not lost)
 *   -1 dropped garbage (overlong sequence, junk byte)
 *
 * A lone ESC (quit key) is detected by the caller with a short sleep plus
 * console op 5 (pending input bytes): no bytes pending means the ESC
 * stands alone. That policy lives in code.c, not here.
 *
 * UTF-8 is structural: cursor motion steps over whole sequences so a
 * multibyte character is never split. Invalid lead bytes count as
 * length 1, so every path makes progress. Display width is 1 per code
 * point (documented simplification: no wcwidth table yet).
 */

enum code_key_kind {
    CODE_K_CHAR,   /* printable code point: ev.ch decoded, ev.raw exact bytes */
    CODE_K_ENTER,  /* \n or \r */
    CODE_K_BSPACE, /* 0x08 or 0x7f */
    CODE_K_TAB,    /* plain Tab: text in the editor, focus flip in the tree */
    CODE_K_FOCUS,  /* ESC[Z (Ctrl+Tab on xterm-likes): flip panel focus */
    CODE_K_UP,
    CODE_K_DOWN,
    CODE_K_LEFT,
    CODE_K_RIGHT,
    CODE_K_HOME,
    CODE_K_END,
    CODE_K_DEL,    /* ESC[3~ */
    CODE_K_ESC,    /* lone Escape */
    CODE_K_CTRL    /* ev.ch is 'a'..'z' (never 'i' == Tab, never 'm') */
};

struct code_ev {
    int kind;              /* enum code_key_kind */
    axys_uint32_t ch;      /* code point (CHAR) or lowercase letter (CTRL) */
    char raw[4];           /* exact CHAR bytes */
    axys_uint32_t raw_len; /* 1..4 for CHAR, 0 otherwise */
    int shift;             /* arrows/home/end arrived shifted */
};

struct code_keydec {
    int state;       /* 0 ground, 1 after ESC, 2 after ESC[, 3 after ESC O */
    int collected;   /* tail bytes stored in seq so far */
    char seq[5];     /* "1;2X" plus NUL room; longer tails are dropped */
    char mb[4];      /* multibyte assembly in ground state */
    int mb_have;     /* bytes stored (lead included) */
    int mb_need;     /* total bytes wanted */
};

static void code_key_reset(struct code_keydec *d)
{
    d->state = 0;
    d->collected = 0;
    d->mb_have = 0;
    d->mb_need = 0;
}

/* Length of the UTF-8 sequence starting with lead byte c (1..4). */
static axys_uint32_t code_utf8_len(unsigned char c)
{
    if (c < 0x80u) {
        return 1;
    }
    if (c >= 0xc2u && c <= 0xdfu) {
        return 2;
    }
    if (c >= 0xe0u && c <= 0xefu) {
        return 3;
    }
    if (c >= 0xf0u && c <= 0xf4u) {
        return 4;
    }
    return 1; /* stray continuation or overlong lead: progress anyway */
}

/* Start offset of the sequence holding text[pos-1] (0 when pos is 0). */
static axys_size_t code_utf8_prev(const char *text, axys_size_t pos)
{
    axys_size_t p;

    if (pos == 0) {
        return 0;
    }
    p = pos - 1;
    /* Back over continuation bytes, at most 3, never past 0. */
    while (p > 0 && ((unsigned char)text[p] & 0xc0u) == 0x80u && pos - p < 4) {
        --p;
    }
    return p;
}

/* Fill a CHAR event from run bytes b[0..n] (1..4). */
static void code_ev_char(struct code_ev *ev, const char *b, axys_uint32_t n)
{
    axys_uint32_t i;

    ev->kind = CODE_K_CHAR;
    ev->ch = 0;
    ev->raw_len = n > 4u ? 4u : n;
    ev->shift = 0;
    for (i = 0; i < ev->raw_len; ++i) {
        ev->raw[i] = b[i];
    }
    if (n >= 1 && ((unsigned char)b[0] & 0x80u) == 0) {
        ev->ch = (axys_uint32_t)(unsigned char)b[0];
    } else {
        /* Minimal decode (search/compare, not rendering). */
        axys_uint32_t v = 0;
        axys_uint32_t k;

        for (k = 0; k < ev->raw_len; ++k) {
            if (k == 0) {
                v = (axys_uint32_t)((unsigned char)b[0] & (0x7fu >> ev->raw_len));
            } else {
                v = (v << 6) | (axys_uint32_t)((unsigned char)b[k] & 0x3fu);
            }
        }
        ev->ch = v;
    }
}

/*
 * Match seq[0..len] (ESC[ tail, no ESC). 1 with ev set, 0 need-more
 * (only for a `1;` prefix that may grow into `1;2X`), -1 garbage.
 */
static int code_match_tail(struct code_ev *ev, const char *seq, int len)
{
    ev->shift = 0;
    if (len == 1) {
        switch (seq[0]) {
        case 'A': ev->kind = CODE_K_UP; return 1;
        case 'B': ev->kind = CODE_K_DOWN; return 1;
        case 'C': ev->kind = CODE_K_RIGHT; return 1;
        case 'D': ev->kind = CODE_K_LEFT; return 1;
        case 'H': ev->kind = CODE_K_HOME; return 1;
        case 'F': ev->kind = CODE_K_END; return 1;
        case 'Z': ev->kind = CODE_K_FOCUS; return 1;
        case '1': case '3': return 0; /* may grow into `1;2X` / `3~` */
        default: return -1;
        }
    }
    if (len == 2 && seq[0] == '3' && seq[1] == '~') {
        ev->kind = CODE_K_DEL;
        return 1;
    }
    if (len == 4 && seq[0] == '1' && seq[1] == ';' && seq[2] == '2') {
        ev->shift = 1;
        switch (seq[3]) {
        case 'A': ev->kind = CODE_K_UP; return 1;
        case 'B': ev->kind = CODE_K_DOWN; return 1;
        case 'C': ev->kind = CODE_K_RIGHT; return 1;
        case 'D': ev->kind = CODE_K_LEFT; return 1;
        case 'H': ev->kind = CODE_K_HOME; return 1;
        case 'F': ev->kind = CODE_K_END; return 1;
        default: return -1;
        }
    }
    if (len == 2 && seq[0] == '1' && seq[1] == ';') {
        return 0; /* may yet become `1;2X` */
    }
    if (len == 3 && seq[0] == '1' && seq[1] == ';' && seq[2] == '2') {
        return 0; /* the final byte (A/B/C/D/H/F) is still coming */
    }
    return -1;
}

static int code_key_feed(struct code_keydec *d, char c, struct code_ev *ev)
{
    unsigned char u = (unsigned char)c;

    /* Defaults: CHAR fills ch/raw/shift; every other event leaves them 0. */
    ev->ch = 0;
    ev->raw_len = 0;
    ev->shift = 0;

    if (d->state == 3) {
        /* After ESC O: H/F on some terminals, anything else is junk. */
        d->state = 0;
        if (c == 'H') {
            ev->kind = CODE_K_HOME;
            ev->shift = 0;
            return 1;
        }
        if (c == 'F') {
            ev->kind = CODE_K_END;
            ev->shift = 0;
            return 1;
        }
        return -1;
    }
    if (d->state == 2) {
        /* After ESC[: one final byte, `3~`, or a `1;2X` modifier tail. */
        if (d->collected < 4) {
            d->seq[d->collected++] = c;
            {
                int r = code_match_tail(ev, d->seq, d->collected);

                if (r > 0) {
                    d->state = 0;
                    d->collected = 0;
                    return r;
                }
                if (r < 0) {
                    d->state = 0;
                    d->collected = 0;
                    return -1;
                }
            }
            return 0;
        }
        d->state = 0;
        d->collected = 0;
        return -1;
    }
    if (d->state == 1) {
        if (c == '[') {
            d->state = 2;
            d->collected = 0;
            return 0;
        }
        if (c == 'O') {
            d->state = 3;
            return 0;
        }
        if (c == 0x1b) {
            return 0; /* ESC ESC: stay armed */
        }
        /* ESC plus anything else: the ESC counts, refeed the follower. */
        d->state = 0;
        ev->kind = CODE_K_ESC;
        ev->shift = 0;
        return 2;
    }
    if (u == 0x1b) {
        d->state = 1;
        d->collected = 0;
        return 0;
    }
    if (d->mb_need > 0) {
        /* Mid-sequence: only continuation bytes belong here. Anything
         * else ends a broken run: the lead counts alone (progress) and
         * the follower is refed by the caller (return 3, like 2). */
        if ((u & 0xc0u) != 0x80u || d->mb_have >= 4) {
            d->mb_need = 0;
            d->mb_have = 0;
            code_ev_char(ev, d->mb, 1);
            return 3;
        }
        d->mb[d->mb_have++] = c;
        if (d->mb_have >= d->mb_need) {
            d->mb_need = 0;
            code_ev_char(ev, d->mb, (axys_uint32_t)d->mb_have);
            d->mb_have = 0;
            return 1;
        }
        return 0;
    }
    if (u == '\r') {
        u = '\n';
        c = '\n';
    }
    if (u == '\n') {
        ev->kind = CODE_K_ENTER;
        ev->shift = 0;
        return 1;
    }
    if (u == 0x08 || u == 0x7f) {
        ev->kind = CODE_K_BSPACE;
        ev->shift = 0;
        return 1;
    }
    if (u == '\t') {
        ev->kind = CODE_K_TAB;
        ev->shift = 0;
        ev->ch = 0;
        return 1;
    }
    if (u < 0x20) {
        /* Control bytes arrive whole in raw mode: Ctrl+letter, lowercase
         * like crossterm. 0x09 and 0x0d are handled above, so 'i' and 'm'
         * never appear here. */
        ev->kind = CODE_K_CTRL;
        ev->ch = (axys_uint32_t)((u - 1u) + 'a');
        ev->shift = 0;
        return 1;
    }
    if (u >= 0x80) {
        d->mb[0] = c;
        d->mb_have = 1;
        d->mb_need = (int)code_utf8_len(u);
        if (d->mb_need <= 1) {
            d->mb_need = 0;
            code_ev_char(ev, d->mb, 1);
            d->mb_have = 0;
            return 1;
        }
        return 0;
    }
    code_ev_char(ev, &c, 1);
    return 1;
}

#endif
