/*
 * The VGA escape decoder (include/axys/csi.h): parameters, saturation,
 * swallowing, aborts and the actions for J/H/f/K/m. Hosted build only.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/axys/csi.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Feed a whole string; return the last DO action seen (or NONE). */
static int feed_all(struct axys_csi *st, const char *s, int *prints, int *shallows)
{
    int action = AXYS_CSI_NONE;

    for (; *s; ++s) {
        int r = axys_csi_feed(st, *s);

        if (r == AXYS_CSI_PRINT) {
            ++*prints;
        } else if (r == AXYS_CSI_SWALLOW) {
            ++*shallows;
        } else {
            action = st->action;
        }
    }
    return action;
}

/* Feed a string, requiring exactly one completion with the given action. */
static void seq(const char *s, int want_action, unsigned a0, unsigned a1)
{
    struct axys_csi st;
    int prints = 0, swallows = 0;
    int action;

    axys_csi_reset(&st);
    action = feed_all(&st, s, &prints, &swallows);
    CHECK(action == want_action);
    CHECK(st.arg0 == a0);
    CHECK(st.arg1 == a1);
    CHECK(prints == 0);
}

int main(void)
{
    struct axys_csi st;
    int prints, swallows;

    /* Plain text passes through untouched. */
    axys_csi_reset(&st);
    prints = swallows = 0;
    CHECK(feed_all(&st, "hello\r\n\t", &prints, &swallows) == AXYS_CSI_NONE);
    CHECK(prints == 8 && swallows == 0);

    /* Erase display, all modes. */
    seq("\x1b[2J", AXYS_CSI_ERASE_DISPLAY, 2, 0);
    seq("\x1b[J", AXYS_CSI_ERASE_DISPLAY, 0, 0);
    seq("\x1b[0J", AXYS_CSI_ERASE_DISPLAY, 0, 0);
    seq("\x1b[1J", AXYS_CSI_ERASE_DISPLAY, 1, 0);
    /* Cursor home and motion. */
    seq("\x1b[H", AXYS_CSI_CURSOR_TO, 0, 0);
    seq("\x1b[f", AXYS_CSI_CURSOR_TO, 0, 0);
    seq("\x1b[5;10H", AXYS_CSI_CURSOR_TO, 5, 10);
    seq("\x1b[5;10f", AXYS_CSI_CURSOR_TO, 5, 10);
    seq("\x1b[;5H", AXYS_CSI_CURSOR_TO, 0, 5);
    /* Erase line. */
    seq("\x1b[K", AXYS_CSI_ERASE_LINE, 0, 0);
    seq("\x1b[1K", AXYS_CSI_ERASE_LINE, 1, 0);
    seq("\x1b[2K", AXYS_CSI_ERASE_LINE, 2, 0);
    /* SGR completes with no action (swallowed, never printed). */
    {
        int p = 0, sw = 0;

        axys_csi_reset(&st);
        CHECK(feed_all(&st, "\x1b[31m", &p, &sw) == AXYS_CSI_NONE);
        CHECK(p == 0 && sw == 4);
    }
    /* Saturation instead of overflow. */
    seq("\x1b[99999J", AXYS_CSI_ERASE_DISPLAY, 9999, 0);
    /* Parameters past the fourth are dropped, the rest still decode. */
    seq("\x1b[1;2;3;4;5H", AXYS_CSI_CURSOR_TO, 1, 2);
    /* Stale slots must not leak: ';5' leaves slot 0 missing, not previous. */
    axys_csi_reset(&st);
    prints = swallows = 0;
    CHECK(feed_all(&st, "\x1b[2J", &prints, &swallows) == AXYS_CSI_ERASE_DISPLAY);
    prints = swallows = 0;
    CHECK(feed_all(&st, "\x1b[;5H", &prints, &swallows) == AXYS_CSI_CURSOR_TO);
    CHECK(st.arg0 == 0 && st.arg1 == 5);
    /* Private '?': aborts, the tail prints as text. */
    {
        int p = 0, sw = 0;

        axys_csi_reset(&st);
        CHECK(feed_all(&st, "\x1b[?25l", &p, &sw) == AXYS_CSI_NONE);
        CHECK(p == 3 && sw == 3);
    }
    /* Stray ESC plus follower: both dropped, text resumes. */
    {
        int p = 0, sw = 0;

        axys_csi_reset(&st);
        CHECK(feed_all(&st, "\x1bxAB", &p, &sw) == AXYS_CSI_NONE);
        CHECK(p == 2 && sw == 2);
    }
    /* ESC ESC stays armed, then a real sequence still runs. */
    seq("\x1b\x1b[2J", AXYS_CSI_ERASE_DISPLAY, 2, 0);
    /* Unknown final byte: swallowed whole, nothing printed. */
    {
        int p = 0, sw = 0;

        axys_csi_reset(&st);
        CHECK(feed_all(&st, "\x1b[9q", &p, &sw) == AXYS_CSI_NONE);
        CHECK(p == 0 && sw == 3);
    }
    /* Unterminated sequence holds state; later bytes finish it. */
    axys_csi_reset(&st);
    prints = swallows = 0;
    CHECK(feed_all(&st, "\x1b[2", &prints, &swallows) == AXYS_CSI_NONE);
    CHECK(prints == 0 && swallows == 3);
    prints = swallows = 0;
    CHECK(feed_all(&st, "J", &prints, &swallows) == AXYS_CSI_ERASE_DISPLAY);
    CHECK(st.arg0 == 2);

    printf("test_csi: ok\n");
    return 0;
}
