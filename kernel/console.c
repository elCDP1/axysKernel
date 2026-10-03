#include "axys/console.h"
#include "axys/serial.h"
#include "axys/vga.h"

void axys_console_init(void)
{
#if CONFIG_AXYS_CONSOLE_VGA
    axys_vga_init();
#endif
#if CONFIG_AXYS_CONSOLE_SERIAL
    axys_serial_init();
#endif
}

void axys_console_putc(char character)
{
#if CONFIG_AXYS_CONSOLE_SERIAL
    axys_serial_putc(character);
#endif
#if CONFIG_AXYS_CONSOLE_VGA
    axys_vga_putc(character);
#else
    (void)character;
#endif
}

void axys_console_write(const char *string)
{
    if (string == AXYS_NULL) {
        return;
    }
    while (*string != '\0') {
        axys_console_putc(*string);
        ++string;
    }
}

void axys_console_write_len(const char *string, axys_size_t length)
{
    if (string == AXYS_NULL) {
        return;
    }
    for (axys_size_t index = 0; index < length; ++index) {
        axys_console_putc(string[index]);
    }
}
