#include "axys/vga.h"
#include "axys/cpu.h"
#include "axys/csi.h"
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
/* Current SGR attribute for glyphs and erased cells (VGA byte: 0-3 fg
 * with bit 3 bright, 4-6 bg). Editors set colours; the shell never does,
 * so this stays 0x07 until something asks. */
static axys_uint8_t cur_attr = 0x07;
/* Alternate screen for fullscreen programs: a saved copy plus the state to
 * restore. Nesting is ignored (first save wins) so a buggy program cannot
 * strand the shell screen. */
static axys_uint16_t alt_buf[VGA_WIDTH * VGA_HEIGHT];
static axys_uint32_t alt_row;
static axys_uint32_t alt_column;
static axys_uint8_t alt_attr;
static int alt_cursor_visible;
static int cursor_visible = 1;
static int in_alt;

/* Cursor register writes are two slow PIO transactions each. During a line of
 * log output they would run once per character; instead we mark the cursor
 * dirty and push it once at end-of-line (or when something else moves it).
 * The visible cursor then trails the write position by at most one line,
 * which is exactly what every buffered terminal does anyway. */
static int cursor_dirty;

static struct axys_csi csi;

static void cursor_update(void)
{
    axys_uint32_t position = cursor_row * VGA_WIDTH + cursor_column;

    axys_outb(VGA_CURSOR_COMMAND, 0x0f);
    axys_outb(VGA_CURSOR_DATA, (axys_uint8_t)(position & 0xff));
    axys_outb(VGA_CURSOR_COMMAND, 0x0e);
    axys_outb(VGA_CURSOR_DATA, (axys_uint8_t)((position >> 8) & 0xff));
    /* Visibility lives in bit 5 of the cursor-start register; the scan
     * lines stay whatever the firmware set (read-modify-write). */
    axys_outb(VGA_CURSOR_COMMAND, 0x0a);
    {
        axys_uint8_t start = axys_inb(VGA_CURSOR_DATA);

        if (cursor_visible) {
            start = (axys_uint8_t)(start & (axys_uint8_t)~0x20u);
        } else {
            start = (axys_uint8_t)(start | 0x20u);
        }
        axys_outb(VGA_CURSOR_DATA, start);
    }
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
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + column] =
            (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)' ');
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
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    for (axys_uint32_t index = 0; index < VGA_WIDTH * VGA_HEIGHT; ++index) {
        vga_buffer[index] = (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)' ');
    }
    cursor_row = 0;
    cursor_column = 0;
    axys_csi_reset(&csi);
    axys_cpu_restore_flags(flags);
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

/* The escape decoder lives in <axys/csi.h> (unit-tested on the host); here
 * only the screen half: erase a range, apply a completed action. */

static void erase_range(axys_uint32_t from_row, axys_uint32_t from_col, axys_uint32_t to_row,
                        axys_uint32_t to_col)
{
    for (axys_uint32_t row = from_row; row <= to_row && row < VGA_HEIGHT; ++row) {
        axys_uint32_t first = (row == from_row) ? from_col : 0;
        axys_uint32_t last = (row == to_row) ? to_col : VGA_WIDTH - 1u;

        if (first >= VGA_WIDTH) {
            continue;
        }
        if (last >= VGA_WIDTH) {
            last = VGA_WIDTH - 1u;
        }
        for (axys_uint32_t column = first; column <= last; ++column) {
            vga_buffer[row * VGA_WIDTH + column] =
                (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)' ');
        }
    }
}

/* 1-based CSI row/column (0 and missing mean 1), clamped to the screen. */
static axys_uint32_t clamp_pos(axys_uint32_t v, axys_uint32_t limit)
{
    if (v == 0u) {
        v = 1u;
    }
    return v - 1u >= limit ? limit - 1u : v - 1u;
}

static void csi_apply(void)
{
    if (csi.action == AXYS_CSI_ERASE_DISPLAY) {
        if (csi.arg0 == 2u) {
            axys_vga_clear();
        } else if (csi.arg0 == 1u) {
            erase_range(0, 0, cursor_row, cursor_column);
        } else {
            erase_range(cursor_row, cursor_column, VGA_HEIGHT - 1u, VGA_WIDTH - 1u);
        }
    } else if (csi.action == AXYS_CSI_CURSOR_TO) {
        cursor_row = clamp_pos(csi.arg0, VGA_HEIGHT);
        cursor_column = clamp_pos(csi.arg1, VGA_WIDTH);
        cursor_update();
    } else if (csi.action == AXYS_CSI_ERASE_LINE) {
        if (csi.arg0 == 2u) {
            erase_range(cursor_row, 0, cursor_row, VGA_WIDTH - 1u);
        } else if (csi.arg0 == 1u) {
            erase_range(cursor_row, 0, cursor_row, cursor_column);
        } else {
            erase_range(cursor_row, cursor_column, cursor_row, VGA_WIDTH - 1u);
        }
    } else if (csi.action == AXYS_CSI_SGR) {
        /* 0 reset, 1 bright, 30-37 fg (bright bit kept), 40-47 bg.
         * ANSI and VGA disagree on red/blue/yellow/cyan, hence the table.
         * Anything else is ignored; all indices stay in range by
         * construction (at most AXYS_CSI_PARAMS, counted, never indexed
         * past the decoded set). */
        static const axys_uint8_t sgr2vga[8] = {0u, 4u, 2u, 6u, 1u, 5u, 3u, 7u};
        axys_uint32_t n = axys_csi_count(&csi);

        for (axys_uint32_t i = 0; i < n; ++i) {
            axys_uint32_t p = axys_csi_raw(&csi, i);

            if (p == 0u) {
                cur_attr = 0x07;
            } else if (p == 1u) {
                cur_attr = (axys_uint8_t)(cur_attr | 0x08u);
            } else if (p >= 30u && p <= 37u) {
                cur_attr = (axys_uint8_t)((cur_attr & 0xf8u) | sgr2vga[p - 30u]);
            } else if (p >= 40u && p <= 47u) {
                cur_attr = (axys_uint8_t)((cur_attr & 0x0fu) |
                                          (axys_uint8_t)(sgr2vga[p - 40u] << 4));
            }
        }
    } else if (csi.action == AXYS_CSI_CURSOR_HIDE) {
        cursor_visible = 0;
        cursor_update();
    } else if (csi.action == AXYS_CSI_CURSOR_SHOW) {
        cursor_visible = 1;
        cursor_update();
    } else if (csi.action == AXYS_CSI_ALT_ON) {
        if (!in_alt) {
            for (axys_uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
                alt_buf[i] = vga_buffer[i];
            }
            alt_row = cursor_row;
            alt_column = cursor_column;
            alt_attr = cur_attr;
            alt_cursor_visible = cursor_visible;
            in_alt = 1;
            cur_attr = 0x07;
            cursor_visible = 1;
            axys_vga_clear();
        }
    } else if (csi.action == AXYS_CSI_ALT_OFF) {
        if (in_alt) {
            for (axys_uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) {
                vga_buffer[i] = alt_buf[i];
            }
            cursor_row = alt_row;
            cursor_column = alt_column;
            cur_attr = alt_attr;
            cursor_visible = alt_cursor_visible;
            in_alt = 0;
            cursor_update();
        }
    }
    /* AXYS_CSI_NONE: accepted, not rendered. */
}

void axys_vga_putc(char character)
{
    /* One character holds interrupts off: putc also runs in IRQ handlers,
     * and an interrupt printing mid-sequence would otherwise steer the
     * shared decoder. Save/restore nests with callers that already hold it. */
    axys_uint64_t flags = axys_cpu_save_flags();
    int step;

    axys_cpu_disable_interrupts();
    step = axys_csi_feed(&csi, character);
    if (step == AXYS_CSI_DO) {
        csi_apply();
    } else if (step == AXYS_CSI_PRINT) {
        if (character == '\n') {
            axys_vga_newline(); /* publishes the cursor itself */
        } else if (character == '\r') {
            cursor_column = 0;
            cursor_dirty = 1;
        } else if (character == '\b') {
        if (cursor_column != 0) {
            --cursor_column;
            vga_buffer[cursor_row * VGA_WIDTH + cursor_column] =
                (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)' ');
            cursor_dirty = 1;
        }
    } else if (character == '\t') {
        axys_uint32_t next_column = (cursor_column / 8 + 1) * 8;
        while (cursor_column < next_column && cursor_column < VGA_WIDTH) {
            vga_buffer[cursor_row * VGA_WIDTH + cursor_column] =
                (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)' ');
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
            /* Other C0 controls (BEL, DEL, ...) have no VGA rendition:
             * swallow them instead of littering '?' glyphs. The serial
             * terminal still receives the raw byte and may beep. */
            axys_cpu_restore_flags(flags);
            return;
        }
        vga_buffer[cursor_row * VGA_WIDTH + cursor_column] =
            (axys_uint16_t)((axys_uint16_t)cur_attr << 8 | (axys_uint16_t)display_character);
        ++cursor_column;
        if (cursor_column == VGA_WIDTH) {
            axys_vga_newline();
        } else {
            cursor_dirty = 1;
        }
    }
    }
    axys_cpu_restore_flags(flags);
}
