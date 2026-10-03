#include "axys/input.h"
#include "axys/cpu.h"
#include "axys/console.h"
#include "axys/interrupts.h"
#include "axys/io.h"
#include "axys/process.h"
#include "axys/sched.h"

#define KBD_DATA 0x60
#define KBD_STATUS 0x64
#define KBD_COMMAND 0x64
#define KBD_STATUS_OUT_FULL 0x01
#define KBD_STATUS_IN_FULL 0x02
#define COM1 0x3f8
#define KBD_IRQ 1
#define COM1_IRQ 4
#define RING_SIZE 256u /* power of two */

static axys_uint8_t ring[RING_SIZE];
static axys_uint32_t ring_head; /* next write */
static axys_uint32_t ring_tail; /* next read */
static axys_uint64_t dropped;
static int input_channel;

/* Modifier state, updated from the IRQ handler only. */
static int shift_down;
static int ctrl_down;
static int caps_lock;
static int extended; /* previous byte was 0xE0 */

/* Scancode set 1 key index -> character. Left unsized so the terminating NUL is
 * stored: the literal is one byte longer than the keys it maps, and an array
 * sized to the literal without room for the NUL is a truncation. */
static const char keymap[] = "\0\033" "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char shiftmap[] = "\0\033" "!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
#define KEYMAP_SIZE (sizeof(keymap) - 1u) /* keys actually mapped */

/* Push from interrupt context (interrupts are already disabled there). */
static void ring_push(char c)
{
    if (c == 3 && axys_process_interrupt_foreground()) {
        return; /* Ctrl-C consumed: it killed the foreground job */
    }
    axys_uint32_t next = (ring_head + 1u) & (RING_SIZE - 1u);

    if (next == ring_tail) {
        ++dropped;
        return;
    }
    ring[ring_head] = (axys_uint8_t)c;
    ring_head = next;
}

static void push_string(const char *text)
{
    while (*text != '\0') {
        ring_push(*text++);
    }
}

static void kbd_handle_scancode(axys_uint8_t code)
{
    int released = (code & 0x80u) != 0;
    axys_uint8_t key = (axys_uint8_t)(code & 0x7fu);

    if (code == 0xe0u) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        if (released) {
            if (key == 0x1du) {
                ctrl_down = 0; /* right Ctrl shares the same modifier state */
            }
            return;
        }
        switch (key) {
        case 0x48: push_string("\033[A"); break; /* up */
        case 0x50: push_string("\033[B"); break; /* down */
        case 0x4d: push_string("\033[C"); break; /* right */
        case 0x4b: push_string("\033[D"); break; /* left */
        case 0x47: push_string("\033[H"); break; /* home */
        case 0x4f: push_string("\033[F"); break; /* end */
        case 0x53: push_string("\033[3~"); break; /* delete */
        case 0x35: ring_push('/'); break;         /* keypad / */
        case 0x1c: ring_push('\n'); break;        /* keypad enter */
        case 0x1d: ctrl_down = 1; break;          /* right ctrl (press) */
        default: break;
        }
        return;
    }

    switch (key) {
    case 0x2a:
    case 0x36:
        shift_down = !released;
        return;
    case 0x1d:
        ctrl_down = !released;
        return;
    case 0x3a:
        if (!released) {
            caps_lock = !caps_lock;
        }
        return;
    default:
        break;
    }
    if (released || key >= KEYMAP_SIZE) {
        return;
    }
    {
        char c = shift_down ? shiftmap[key] : keymap[key];

        if (c == '\0') {
            return;
        }
        if (caps_lock && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c = (char)(c ^ 0x20);
        }
        if (ctrl_down && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c = (char)(c & 0x1f);
        }
        ring_push(c);
    }
}

static void kbd_irq(struct axys_interrupt_frame *frame)
{
    unsigned budget = 16; /* bounded: a stuck controller must not wedge the IRQ */

    (void)frame;
    while ((axys_inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) != 0 && budget-- != 0) {
        kbd_handle_scancode(axys_inb(KBD_DATA));
    }
    axys_task_wake(&input_channel);
}

static void serial_irq(struct axys_interrupt_frame *frame)
{
    unsigned budget = 64;

    (void)frame;
    while ((axys_inb(COM1 + 5) & 0x01u) != 0 && budget-- != 0) {
        char c = (char)axys_inb(COM1);

        if (c == '\r') {
            c = '\n';
        } else if (c == 0x7f) {
            c = '\b';
        }
        ring_push(c);
    }
    axys_task_wake(&input_channel);
}

static int kbd_wait_input_clear(void)
{
    for (unsigned i = 0; i < 100000u; ++i) {
        if ((axys_inb(KBD_STATUS) & KBD_STATUS_IN_FULL) == 0) {
            return 1;
        }
    }
    return 0;
}

static int kbd_wait_output_full(void)
{
    for (unsigned i = 0; i < 100000u; ++i) {
        if ((axys_inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) != 0) {
            return 1;
        }
    }
    return 0;
}

static void kbd_flush(void)
{
    for (unsigned i = 0; i < 64u && (axys_inb(KBD_STATUS) & KBD_STATUS_OUT_FULL) != 0; ++i) {
        (void)axys_inb(KBD_DATA);
    }
}

static int i8042_init(void)
{
    axys_uint8_t config;

    if (axys_inb(KBD_STATUS) == 0xffu) {
        return -1; /* floating bus: no controller */
    }
    axys_outb(KBD_COMMAND, 0xad); /* disable the keyboard port while configuring */
    kbd_flush();
    axys_outb(KBD_COMMAND, 0x20); /* read configuration byte */
    if (!kbd_wait_output_full()) {
        return -1;
    }
    config = axys_inb(KBD_DATA);
    config = (axys_uint8_t)((config | 0x01u | 0x40u) & (axys_uint8_t)~0x10u); /* IRQ1, translation, clock on */
    if (!kbd_wait_input_clear()) {
        return -1;
    }
    axys_outb(KBD_COMMAND, 0x60);
    if (!kbd_wait_input_clear()) {
        return -1;
    }
    axys_outb(KBD_DATA, config);
    if (!kbd_wait_input_clear()) {
        return -1;
    }
    axys_outb(KBD_COMMAND, 0xae); /* enable the keyboard port */
    if (kbd_wait_input_clear()) {
        axys_outb(KBD_DATA, 0xf4); /* enable scanning; the ACK is ignored by the decoder */
    }
    return 0;
}

int axys_input_init(void)
{
    int status = i8042_init();

    ring_head = ring_tail = 0;
    axys_irq_register(KBD_IRQ, kbd_irq);
    axys_irq_register(COM1_IRQ, serial_irq);
    axys_outb(COM1 + 1, 0x01); /* UART: interrupt on received data */
    (void)axys_inb(COM1);      /* drop anything already pending */
    if (status == 0) {
        axys_pic_unmask(KBD_IRQ);
    }
    axys_pic_unmask(COM1_IRQ);
    return status;
}

int axys_input_getc_nonblock(void)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    int c = -1;

    axys_cpu_disable_interrupts();
    if (ring_tail != ring_head) {
        c = ring[ring_tail];
        ring_tail = (ring_tail + 1u) & (RING_SIZE - 1u);
    }
    axys_cpu_restore_flags(flags);
    return c;
}

int axys_input_getc(void)
{
    for (;;) {
        axys_uint64_t flags = axys_cpu_save_flags();
        int c;

        axys_cpu_disable_interrupts();
        c = -1;
        if (ring_tail != ring_head) {
            c = ring[ring_tail];
            ring_tail = (ring_tail + 1u) & (RING_SIZE - 1u);
        } else {
            axys_task_block(&input_channel);
        }
        axys_cpu_restore_flags(flags);
        if (c >= 0) {
            return c;
        }
    }
}

axys_size_t axys_input_readline(char *buffer, axys_size_t size)
{
    axys_size_t length = 0;

    if (buffer == AXYS_NULL || size == 0) {
        return 0;
    }
    for (;;) {
        int c = axys_input_getc();

        if (c == '\n') {
            axys_console_putc('\n');
            break;
        }
        if (c == '\b') {
            if (length > 0) {
                --length;
                axys_console_write("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7f && length + 1 < size) {
            buffer[length++] = (char)c;
            axys_console_putc((char)c);
        }
    }
    buffer[length] = '\0';
    return length;
}

axys_uint64_t axys_input_dropped(void)
{
    return dropped;
}
