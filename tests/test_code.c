/*
 * The /bin/code core (user/code_key.h, user/code_buf.h): key decoding,
 * UTF-8 structure, document ops and their loud refusals. Hosted build only.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/code_buf.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Feed bytes; expect silence (0) until exactly one event of a kind. */
static void key(const char *bytes, int want_kind)
{
    struct code_keydec d;
    struct code_ev ev;

    code_key_reset(&d);
    for (; *bytes; ++bytes) {
        int r = code_key_feed(&d, *bytes, &ev);

        if (r == 1) {
            CHECK(ev.kind == want_kind);
            CHECK(bytes[1] == '\0'); /* single event per input */
            return;
        }
        CHECK(r == 0);
    }
    fprintf(stderr, "no event for input\n");
    exit(1);
}

static void key_char(const char *bytes, unsigned want_ch, unsigned want_len)
{
    struct code_keydec d;
    struct code_ev ev;
    const char *p = bytes;

    code_key_reset(&d);
    for (;;) {
        int r = code_key_feed(&d, *p, &ev);

        if (r == 1) {
            break;
        }
        CHECK(r == 0);
        ++p;
    }
    CHECK(ev.kind == CODE_K_CHAR && ev.ch == want_ch && ev.raw_len == want_len);
}

int main(void)
{
    struct code_keydec d;
    struct code_ev ev;
    struct code_doc doc;
    int r;

    /* UTF-8 lengths, including invalid leads (progress, never 0). */
    CHECK(code_utf8_len('a') == 1);
    CHECK(code_utf8_len(0xc3) == 2);
    CHECK(code_utf8_len(0xe2) == 3);
    CHECK(code_utf8_len(0xf0) == 4);
    CHECK(code_utf8_len(0x80) == 1);
    CHECK(code_utf8_len(0xff) == 1);
    /* Prev finds sequence starts, clamps at 0. */
    {
        const char *s = "a\xc3\xa9\xe2\x82\xac"; /* a, U+00E9, U+20AC */
        CHECK(code_utf8_prev(s, 0) == 0);
        CHECK(code_utf8_prev(s, 1) == 0);
        CHECK(code_utf8_prev(s, 2) == 1);
        CHECK(code_utf8_prev(s, 3) == 1);
        CHECK(code_utf8_prev(s, 4) == 3);
        CHECK(code_utf8_prev(s, 6) == 3);
    }

    /* Single bytes. */
    key("a", CODE_K_CHAR);
    key("\n", CODE_K_ENTER);
    key("\r", CODE_K_ENTER);
    key("\x08", CODE_K_BSPACE);
    key("\x7f", CODE_K_BSPACE);
    key("\t", CODE_K_TAB);
    /* Arrows, home/end both spellings, delete. */
    key("\x1b[A", CODE_K_UP);
    key("\x1b[B", CODE_K_DOWN);
    key("\x1b[C", CODE_K_RIGHT);
    key("\x1b[D", CODE_K_LEFT);
    key("\x1b[H", CODE_K_HOME);
    key("\x1b[F", CODE_K_END);
    key("\x1bOH", CODE_K_HOME);
    key("\x1bOF", CODE_K_END);
    key("\x1b[3~", CODE_K_DEL);
    key("\x1b[Z", CODE_K_FOCUS);
    /* Shifted arrows. */
    code_key_reset(&d);
    {
        const char *s = "\x1b[1;2A";
        int done = 0;

        for (; *s; ++s) {
            r = code_key_feed(&d, *s, &ev);
            if (r == 1) {
                done = 1;
            } else {
                CHECK(r == 0);
            }
        }
        CHECK(done && ev.kind == CODE_K_UP && ev.shift == 1);
    }
    /* Ctrl letters (never plain Tab/Ctrl+M: those are TAB/ENTER). */
    code_key_reset(&d);
    CHECK(code_key_feed(&d, 0x03, &ev) == 1 && ev.kind == CODE_K_CTRL && ev.ch == 'c');
    code_key_reset(&d);
    CHECK(code_key_feed(&d, 0x13, &ev) == 1 && ev.kind == CODE_K_CTRL && ev.ch == 's');
    /* Multibyte chars assemble whole. */
    key_char("\xc3\xa9", 0xe9, 2);
    key_char("\xe2\x82\xac", 0x20ac, 3);
    /* Broken run: lead alone (3), then the follower counts. */
    code_key_reset(&d);
    CHECK(code_key_feed(&d, '\xc3', &ev) == 0);
    CHECK(code_key_feed(&d, 'x', &ev) == 3 && ev.kind == CODE_K_CHAR && ev.raw_len == 1);
    CHECK(code_key_feed(&d, 'x', &ev) == 1 && ev.kind == CODE_K_CHAR && ev.ch == 'x');
    /* Garbage sequences die without events. */
    code_key_reset(&d);
    CHECK(code_key_feed(&d, 0x1b, &ev) == 0);
    CHECK(code_key_feed(&d, '[', &ev) == 0);
    CHECK(code_key_feed(&d, 'X', &ev) == -1);
    code_key_reset(&d);
    CHECK(code_key_feed(&d, 0x1b, &ev) == 0);
    CHECK(code_key_feed(&d, '[', &ev) == 0);
    CHECK(code_key_feed(&d, '9', &ev) == -1);
    /* ESC plus letter: ESC event first (2), letter refed after. */
    code_key_reset(&d);
    CHECK(code_key_feed(&d, 0x1b, &ev) == 0);
    CHECK(code_key_feed(&d, 'q', &ev) == 2 && ev.kind == CODE_K_ESC);
    CHECK(code_key_feed(&d, 'q', &ev) == 1 && ev.kind == CODE_K_CHAR && ev.ch == 'q');

    /* Document: load, index, motions. */
    memset(&doc, 0, sizeof(doc));
    CHECK(doc_load(&doc, "ab\nc\xc3\xa9\n\nlast", 12) == 0);
    CHECK(doc.nlines == 4);
    CHECK(doc.line_off[0] == 0 && doc.line_off[1] == 3 && doc.line_off[2] == 7 &&
          doc.line_off[3] == 8);
    CHECK(doc.cursor == 0 && doc.dirty == 0);
    /* Right across ASCII then a 2-byte char: never lands mid-sequence. */
    doc_right(&doc);
    doc_right(&doc);
    CHECK(doc.cursor == 2); /* on the \n byte */
    doc_left(&doc);
    CHECK(doc.cursor == 1 && doc.goal == 1);
    doc_right(&doc);
    CHECK(doc.cursor == 2);
    doc_right(&doc);
    CHECK(doc.cursor == 3); /* line 1 start */
    doc_right(&doc);
    doc_right(&doc); /* over U+00E9 whole */
    CHECK(doc.cursor == 6);
    CHECK(doc.goal == 2);
    /* Down keeps the goal column, clamped at short lines. */
    doc_down(&doc); /* line 2 is empty */
    CHECK(doc.cursor == 7 && doc.goal == 2);
    doc_down(&doc); /* "last", col 2 */
    CHECK(doc.cursor == 10);
    doc_up(&doc);
    CHECK(doc.cursor == 7);
    doc_home(&doc);
    CHECK(doc.cursor == 7 && doc.goal == 0);
    doc_end(&doc);
    CHECK(doc.cursor == 7); /* empty line: home == end */
    /* Backspace deletes the whole sequence before the cursor. */
    doc.cursor = 6;
    CHECK(doc_backspace(&doc) == 0 && doc.len == 10 && doc.dirty == 1);
    CHECK(doc.cursor == 4 && memcmp(doc.text + 3, "c\n", 2) == 0);
    /* Delete at cursor removes the sequence there ('c' gone). */
    doc.cursor = 3;
    CHECK(doc_delete(&doc) == 0);
    CHECK(doc.len == 9 && memcmp(doc.text, "ab\n\n\nlast", 9) == 0);
    /* Insert grows, marks dirty, refuses past the caps. */
    CHECK(doc_insert(&doc, "XY", 2) == 0 && doc.cursor == 5);
    {
        char big[CODE_MAX_FILE];

        memset(big, 'z', sizeof(big));
        CHECK(doc_insert(&doc, big, sizeof(big)) == -1);
        CHECK(doc_load(&doc, big, sizeof(big)) == 0);
        CHECK(doc_insert(&doc, "!", 1) == -1);
    }
    CHECK(doc_load(&doc, "", 0) == 0 && doc.nlines == 1 && doc.len == 0);
    CHECK(doc_backspace(&doc) == 0 && doc_delete(&doc) == 0); /* no-op edges */
    /* Search forward, no wrap. */
    CHECK(doc_load(&doc, "foo bar foo", 11) == 0);
    CHECK(doc_find_from(&doc, "foo", 0) == 0);
    CHECK(doc_find_from(&doc, "foo", 1) == 8);
    CHECK(doc_find_from(&doc, "zzz", 0) == -1);
    CHECK(doc_find_from(&doc, "", 0) == -1);

    printf("test_code: ok\n");
    return 0;
}
