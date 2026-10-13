#ifndef AXYS_CODE_BUF_H
#define AXYS_CODE_BUF_H

#include "axys/types.h"
#include "code_key.h"

/*
 * Document for /bin/code: flat text plus a line index. The safety rule
 * (what Rust ownership would give us) is structural: line_off[] is only
 * ever written by doc_rebuild(), which runs after every mutation, so the
 * index and the text can never disagree. Every operation bounds-checks
 * and refuses instead of truncating: too big a file fails the OPEN, a
 * too-long edit fails the KEYSTROKE, never silently.
 *
 * Limits: 64 KiB files, 4096 lines. Cursor is a byte offset that always
 * sits on a UTF-8 boundary (every motion uses code_utf8_*).
 */

#define CODE_MAX_FILE 65536u
#define CODE_MAX_LINES 4096u

struct code_doc {
    char text[CODE_MAX_FILE + 1u];
    axys_size_t len;                          /* text[len] == '\0' always */
    axys_size_t line_off[CODE_MAX_LINES];     /* byte offset per line */
    axys_uint32_t nlines;
    int dirty;
    axys_size_t cursor;                       /* byte offset, UTF-8 boundary */
    axys_size_t goal;                         /* preferred column (chars) */
};

/* Recompute the whole line index from text. The ONLY writer of line_off. */
static void doc_rebuild(struct code_doc *d)
{
    axys_size_t i;
    axys_uint32_t n = 1;

    d->line_off[0] = 0;
    for (i = 0; i < d->len && n < CODE_MAX_LINES; ++i) {
        if (d->text[i] == '\n') {
            d->line_off[n++] = i + 1;
        }
    }
    d->nlines = n;
    if (d->cursor > d->len) {
        d->cursor = d->len;
    }
}

/* Load n bytes (already read by the caller): 0 ok, -1 too big. */
static int doc_load(struct code_doc *d, const char *data, axys_size_t n)
{
    axys_uint32_t lines = 1;
    axys_size_t i;

    if (n > CODE_MAX_FILE) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (data[i] == '\n') {
            ++lines;
        }
    }
    /* Trailing newline does not start a visible line, but keep one slot
     * of slack so the count stays exact either way. */
    if (lines > CODE_MAX_LINES) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        d->text[i] = data[i];
    }
    d->text[n] = '\0';
    d->len = n;
    d->dirty = 0;
    d->cursor = 0;
    d->goal = 0;
    doc_rebuild(d);
    return 0;
}

/* Byte length of line l (without '\n'), and its start offset. */
static axys_size_t doc_line_span(const struct code_doc *d, axys_uint32_t l,
                                 axys_size_t *start_out)
{
    axys_size_t start = d->line_off[l];
    axys_size_t end;

    if (l + 1 < d->nlines) {
        end = d->line_off[l + 1];
    } else {
        end = d->len;
    }
    if (end > start && d->text[end - 1] == '\n') {
        --end;
    }
    *start_out = start;
    return end - start;
}

/* Which line holds byte offset pos (clamped). */
static axys_uint32_t doc_line_of(const struct code_doc *d, axys_size_t pos)
{
    axys_uint32_t lo = 0;
    axys_uint32_t hi = d->nlines;

    if (d->nlines == 0) {
        return 0;
    }
    while (lo + 1 < hi) {
        axys_uint32_t mid = lo + (hi - lo) / 2;

        if (d->line_off[mid] <= pos) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Character (code point) count of bytes s[0..n]. */
static axys_size_t doc_chars(const char *s, axys_size_t n)
{
    axys_size_t chars = 0;
    axys_size_t i = 0;

    while (i < n) {
        i += code_utf8_len((unsigned char)s[i]);
        ++chars;
    }
    return chars;
}

/* Byte offset of the character `col` chars into line l (clamped to end). */
static axys_size_t doc_col_to_off(const struct code_doc *d, axys_uint32_t l,
                                  axys_size_t col)
{
    axys_size_t start;
    axys_size_t span = doc_line_span(d, l, &start);
    axys_size_t off = start;
    axys_size_t c = 0;

    while (c < col && off < start + span) {
        off += code_utf8_len((unsigned char)d->text[off]);
        ++c;
    }
    return off;
}

/* Insert run b[0..n] at the cursor: 0 ok, -1 would overflow bytes or
 * lines (loud refusal, never a silent truncation). */
static int doc_insert(struct code_doc *d, const char *b, axys_size_t n)
{
    axys_size_t i;
    axys_size_t newlines = 0;

    if (n == 0) {
        return 0;
    }
    if (d->len + n > CODE_MAX_FILE) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (b[i] == '\n') {
            ++newlines;
        }
    }
    if ((axys_size_t)d->nlines + newlines > CODE_MAX_LINES) {
        return -1;
    }
    for (i = d->len; i > d->cursor; --i) {
        d->text[i + n - 1] = d->text[i - 1];
    }
    for (i = 0; i < n; ++i) {
        d->text[d->cursor + i] = b[i];
    }
    d->len += n;
    d->text[d->len] = '\0';
    d->cursor += n;
    d->dirty = 1;
    doc_rebuild(d);
    return 0;
}

/* Delete the code point before the cursor (backspace). 0 ok incl. no-op. */
static int doc_backspace(struct code_doc *d)
{
    axys_size_t prev;
    axys_size_t n;

    if (d->cursor == 0) {
        return 0;
    }
    prev = code_utf8_prev(d->text, d->cursor);
    n = d->cursor - prev;
    for (axys_size_t i = d->cursor; i < d->len; ++i) {
        d->text[i - n] = d->text[i];
    }
    d->len -= n;
    d->text[d->len] = '\0';
    d->cursor = prev;
    d->dirty = 1;
    doc_rebuild(d);
    return 0;
}

/* Delete the code point at the cursor (Delete key). 0 ok incl. no-op. */
static int doc_delete(struct code_doc *d)
{
    axys_size_t n;

    if (d->cursor >= d->len) {
        return 0;
    }
    n = code_utf8_len((unsigned char)d->text[d->cursor]);
    if (d->cursor + n > d->len) {
        n = d->len - d->cursor;
    }
    for (axys_size_t i = d->cursor + n; i < d->len; ++i) {
        d->text[i - n] = d->text[i];
    }
    d->len -= n;
    d->text[d->len] = '\0';
    d->dirty = 1;
    doc_rebuild(d);
    return 0;
}

/* Motions. Every one ends on a UTF-8 boundary with goal refreshed,
 * except up/down, which chase the goal column. */
static void doc_left(struct code_doc *d)
{
    axys_size_t start;
    axys_uint32_t l;

    d->cursor = code_utf8_prev(d->text, d->cursor);
    l = doc_line_of(d, d->cursor);
    (void)doc_line_span(d, l, &start);
    d->goal = doc_chars(d->text + start, d->cursor - start);
}

static void doc_right(struct code_doc *d)
{
    axys_size_t start;
    axys_uint32_t l;

    if (d->cursor < d->len) {
        d->cursor += code_utf8_len((unsigned char)d->text[d->cursor]);
        if (d->cursor > d->len) {
            d->cursor = d->len;
        }
    }
    l = doc_line_of(d, d->cursor);
    (void)doc_line_span(d, l, &start);
    d->goal = doc_chars(d->text + start, d->cursor - start);
}

static void doc_home(struct code_doc *d)
{
    axys_uint32_t l = doc_line_of(d, d->cursor);

    d->cursor = d->line_off[l];
    d->goal = 0;
}

static void doc_end(struct code_doc *d)
{
    axys_size_t start;
    axys_uint32_t l = doc_line_of(d, d->cursor);
    axys_size_t span = doc_line_span(d, l, &start);

    d->cursor = start + span;
    d->goal = doc_chars(d->text + start, span);
}

static void doc_up(struct code_doc *d)
{
    axys_uint32_t l = doc_line_of(d, d->cursor);

    if (l == 0) {
        doc_home(d);
        return;
    }
    d->cursor = doc_col_to_off(d, l - 1, d->goal);
}

static void doc_down(struct code_doc *d)
{
    axys_uint32_t l = doc_line_of(d, d->cursor);

    if (l + 1 >= d->nlines) {
        doc_end(d);
        return;
    }
    d->cursor = doc_col_to_off(d, l + 1, d->goal);
}

/* Forward substring search from byte `from` (no wrap): offset or -1. */
static long doc_find_from(const struct code_doc *d, const char *q, axys_size_t from)
{
    axys_size_t qlen = 0;

    while (q[qlen] != '\0') {
        ++qlen;
    }
    if (qlen == 0 || qlen > d->len) {
        return -1;
    }
    for (axys_size_t i = from; i + qlen <= d->len; ++i) {
        axys_size_t k = 0;

        while (k < qlen && d->text[i + k] == q[k]) {
            ++k;
        }
        if (k == qlen) {
            return (long)i;
        }
    }
    return -1;
}

#endif
