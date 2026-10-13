/*
 * The real VGA text driver (kernel/vga.c) against an mmap()'d 0xb8000 plus
 * stubbed port I/O: text, scrolling, clear/home/cursor/erase escapes and the
 * graceful swallowing of anything else. Hosted build only.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "axys/vga.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define WIDTH 80
#define HEIGHT 25
#define BLANK ((uint16_t)0x0720u)

unsigned stub_crtc_cmd;
unsigned stub_cursor_addr;
unsigned stub_cursor_shape_in = 0x0du; /* firmware shape: only bit 5 is ours */
unsigned stub_cursor_shape_out;

static volatile uint16_t *scr;

static void puts_vga(const char *s)
{
    for (; *s; ++s) {
        axys_vga_putc(*s);
    }
}

static void expect_attr(unsigned row, unsigned col, char ch, unsigned attr);

static void expect_cell(unsigned row, unsigned col, char ch)
{
    expect_attr(row, col, ch, 0x07);
}

static void expect_attr(unsigned row, unsigned col, char ch, unsigned attr)
{
    uint16_t want = (uint16_t)((attr << 8) | (unsigned char)ch);

    if (scr[row * WIDTH + col] != want) {
        fprintf(stderr, "cell (%u,%u) is 0x%04x, wanted 0x%04x\n", row, col, scr[row * WIDTH + col],
                want);
        exit(1);
    }
}

static void expect_blank(void)
{
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
        if (scr[i] != BLANK) {
            fprintf(stderr, "cell %u is 0x%04x, wanted blank\n", i, scr[i]);
            exit(1);
        }
    }
}

int main(void)
{
    void *p = mmap((void *)0xb8000, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    CHECK(p != MAP_FAILED);
    scr = (volatile uint16_t *)p;

    /* init clears the screen and homes the cursor */
    memset((void *)scr, 0xaa, WIDTH * HEIGHT * 2);
    axys_vga_init();
    expect_blank();
    axys_vga_flush();
    CHECK(stub_cursor_addr == 0);

    /* plain text lands, cursor follows after a flush */
    puts_vga("Hi");
    expect_cell(0, 0, 'H');
    expect_cell(0, 1, 'i');
    expect_cell(0, 2, ' ');
    axys_vga_flush();
    CHECK(stub_cursor_addr == 2);

    /* ESC [ 2 J really clears everything and homes */
    puts_vga("abcdefgh");
    puts_vga("\x1b[2J");
    expect_blank();
    CHECK(stub_cursor_addr == 0);

    /* cursor motion, 1-based and clamped */
    puts_vga("\x1b[5;10H");
    CHECK(stub_cursor_addr == 4 * WIDTH + 9);
    puts_vga("X");
    expect_cell(4, 9, 'X');
    puts_vga("\x1b[H");
    CHECK(stub_cursor_addr == 0);
    puts_vga("\x1b[999;999H");
    CHECK(stub_cursor_addr == 24 * WIDTH + 79);

    /* erase line parts */
    puts_vga("\x1b[1;1Habcdefgh");
    axys_vga_flush();
    CHECK(stub_cursor_addr == 8);
    puts_vga("\x1b[1;4H");
    CHECK(stub_cursor_addr == 3);
    puts_vga("\x1b[K");
    expect_cell(0, 0, 'a');
    expect_cell(0, 2, 'c');
    expect_cell(0, 3, ' ');
    expect_cell(0, 79, ' ');
    puts_vga("\x1b[1;1H");
    puts_vga("\x1b[2K");
    for (unsigned c = 0; c < WIDTH; ++c) {
        expect_cell(0, c, ' ');
    }

    /* SGR colours land on the cells: fg, then bg, then reset */
    puts_vga("\x1b[2J");
    puts_vga("a\x1b[31mb\x1b[44mc\x1b[0md");
    expect_attr(0, 0, 'a', 0x07);
    expect_attr(0, 1, 'b', 0x04);
    expect_attr(0, 2, 'c', 0x14);
    expect_attr(0, 3, 'd', 0x07);
    expect_cell(0, 4, ' ');
    /* bold keeps working after a colour, erase uses the current colour */
    puts_vga("\x1b[1;32m");
    puts_vga("e");
    expect_attr(0, 4, 'e', 0x0a);
    puts_vga("\x1b[2K");
    for (unsigned c = 0; c < WIDTH; ++c) {
        uint16_t cell = scr[c];

        if ((cell & 0xff00u) != 0x0a00u || (char)cell != ' ') {
            fprintf(stderr, "cell %u is 0x%04x, wanted green blank\n", c, cell);
            exit(1);
        }
    }
    puts_vga("\x1b[0m");

    /* cursor hide/show keeps the firmware scan lines, flips only bit 5 */
    puts_vga("\x1b[?25l");
    CHECK(stub_cursor_shape_out == (0x0du | 0x20u));
    puts_vga("\x1b[?25h");
    CHECK(stub_cursor_shape_out == 0x0du);

    /* alternate screen saves, clears, and restores everything */
    puts_vga("\x1b[2J");
    puts_vga("\x1b[31mSHELL");
    puts_vga("\x1b[1;1H");
    puts_vga("\x1b[?1049h");
    expect_cell(0, 0, ' ');
    puts_vga("EDITOR");
    expect_attr(0, 0, 'E', 0x07);
    puts_vga("\x1b[?1049l");
    expect_attr(0, 0, 'S', 0x04);
    expect_attr(0, 1, 'H', 0x04);
    CHECK(stub_cursor_addr == 0);
    /* nested enter is ignored: the one exit still restores */
    puts_vga("\x1b[?1049h");
    puts_vga("X");
    puts_vga("\x1b[?1049h");
    puts_vga("Y");
    expect_attr(0, 0, 'X', 0x07);
    expect_attr(0, 1, 'Y', 0x07);
    puts_vga("\x1b[?1049l");
    expect_attr(0, 0, 'S', 0x04);
    /* exit without enter is a no-op */
    puts_vga("\x1b[?1049l");
    expect_attr(0, 0, 'S', 0x04);
    puts_vga("\x1b[0m");

    /* stray ESC plus follower vanish, text resumes */
    puts_vga("\x1b[2J");
    puts_vga("a\x1bzb");
    expect_cell(0, 0, 'a');
    expect_cell(0, 1, 'b');
    expect_cell(0, 2, ' ');

    /* scrolling still ends on the last row */
    puts_vga("\x1b[2J");
    for (int i = 0; i < 30; ++i) {
        axys_vga_putc('\n');
    }
    CHECK(stub_cursor_addr == 24 * WIDTH);

    printf("test_vga: ok\n");
    return 0;
}
