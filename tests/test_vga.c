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

static volatile uint16_t *scr;

static void puts_vga(const char *s)
{
    for (; *s; ++s) {
        axys_vga_putc(*s);
    }
}

static void expect_cell(unsigned row, unsigned col, char ch)
{
    uint16_t want = (uint16_t)(0x0700u | (unsigned char)ch);

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

    /* SGR colours are swallowed, never printed */
    puts_vga("\x1b[2J");
    puts_vga("a\x1b[31mb");
    expect_cell(0, 0, 'a');
    expect_cell(0, 1, 'b');
    expect_cell(0, 2, ' ');

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
