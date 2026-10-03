#include "axys/vga.h"
#include "axys/io.h"

#define VGA_ADDRESS 0xb8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ATTRIBUTE 0x0700
#define VGA_CURSOR_COMMAND 0x3d4
#define VGA_CURSOR_DATA 0x3d5

static volatile axys_uint16_t *vga_buffer;
static axys_uint32_t cursor_row;
static axys_uint32_t cursor_column;

/* Cursor register writes are two slow PIO transactions each. During a line of
 * log output they would run once per character; instead we mark the cursor
 * dirty and push it once at end-of-line (or when something else moves it).
 * The visible cursor then trails the write position by at most one line,
 * which is exactly what every buffered terminal does anyway. */
static int cursor_dirty;

static void cursor_update(void)
{
    axys_uint32_t position = cursor_row * VGA_WIDTH + cursor_column;

    axys_outb(VGA_CURSOR_COMMAND, 0x0f);
    axys_outb(VGA_CURSOR_DATA, (axys_uint8_t)(position & 0xff));
    axys_outb(VGA_CURSOR_COMMAND, 0x0e);
    axys_outb(VGA_CURSOR_DATA, (axys_uint8_t)((position >> 8) & 0xff));
    cursor_dirty = 0;
}

void axys_vga_flush(void)
{
    if (cursor_dirty) {
        cursor_update();
    }
}

static void scroll(void)
{
    /* Row copies: 160 bytes per row beats 80 word-at-a-time stores on the
     * MMIO path; the trailing loop still clears the last row explicitly so
     * no stale glyph survives even if a copy primitive were ever removed. */
    for (axys_uint32_t row = 1; row < VGA_HEIGHT; ++row) {
        volatile axys_uint16_t *dst = &vga_buffer[(row - 1) * VGA_WIDTH];
        const volatile axys_uint16_t *src = &vga_buffer[row * VGA_WIDTH];

        for (axys_uint32_t column = 0; column < VGA_WIDTH; ++column) {
            dst[column] = src[column];
        }
    }
    for (axys_uint32_t column = 0; column < VGA_WIDTH; ++column) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + column] = VGA_ATTRIBUTE | ' ';
    }
    cursor_row = VGA_HEIGHT - 1;
}

void axys_vga_init(void)
{
    vga_buffer = (volatile axys_uint16_t *)(axys_uintptr_t)VGA_ADDRESS;
    cursor_row = 0;
    cursor_column = 0;
    cursor_dirty = 0;
    axys_vga_clear();
}

void axys_vga_clear(void)
{
    for (axys_uint32_t index = 0; index < VGA_WIDTH * VGA_HEIGHT; ++index) {
        vga_buffer[index] = VGA_ATTRIBUTE | ' ';
    }
    cursor_row = 0;
    cursor_column = 0;
    cursor_update();
}

void axys_vga_newline(void)
{
    cursor_column = 0;
    ++cursor_row;
    if (cursor_row >= VGA_HEIGHT) {
        scroll();
    }
    /* End of line: the natural, cheap place to publish the cursor. */
    cursor_update();
}

void axys_vga_putc(char character)
{
    if (character == '\n') {
        axys_vga_newline(); /* publishes the cursor itself */
    } else if (character == '\r') {
        cursor_column = 0;
        cursor_dirty = 1;
    } else if (character == '\b') {
        if (cursor_column != 0) {
            --cursor_column;
            vga_buffer[cursor_row * VGA_WIDTH + cursor_column] = VGA_ATTRIBUTE | ' ';
            cursor_dirty = 1;
        }
    } else if (character == '\t') {
        axys_uint32_t next_column = (cursor_column / 8 + 1) * 8;
        while (cursor_column < next_column && cursor_column < VGA_WIDTH) {
            vga_buffer[cursor_row * VGA_WIDTH + cursor_column] = VGA_ATTRIBUTE | ' ';
            ++cursor_column;
        }
        if (cursor_column == VGA_WIDTH) {
            axys_vga_newline();
        } else {
            cursor_dirty = 1;
        }
    } else {
        axys_uint8_t display_character = (axys_uint8_t)character;
        if (display_character < 0x20 || display_character > 0x7e) {
            display_character = '?';
        }
        vga_buffer[cursor_row * VGA_WIDTH + cursor_column] = VGA_ATTRIBUTE | display_character;
        ++cursor_column;
        if (cursor_column == VGA_WIDTH) {
            axys_vga_newline();
        } else {
            cursor_dirty = 1;
        }
    }
}
