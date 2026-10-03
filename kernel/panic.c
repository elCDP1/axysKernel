#include "axys/panic.h"
#include "axys/cpu.h"
#include "axys/console.h"
#include "axys/printf.h"
#if CONFIG_AXYS_CONSOLE_VGA
#include "axys/vga.h"
#endif

AXYS_NORETURN void axys_panic(const char *message)
{
    axys_cpu_disable_interrupts();
    axys_printf("panic: %s\n", message == AXYS_NULL ? "unknown failure" : message);
#if CONFIG_AXYS_CONSOLE_VGA
    /* Publish any pending cursor position before halting forever: the lazy
     * cursor update only fires at end-of-line, and a panic can stop mid-line
     * with the hardware cursor lagging behind the last glyph written. */
    axys_vga_flush();
#endif
    for (;;) {
        axys_cpu_halt();
    }
}

AXYS_NORETURN void axys_panic_format(const char *format, ...)
{
    char buffer[256];
    axys_va_list arguments;
    int length;

    __builtin_va_start(arguments, format);
    length = axys_vsnprintf(buffer, sizeof(buffer), format, arguments);
    __builtin_va_end(arguments);
    if (length < 0) {
        buffer[0] = '\0';
    }
    axys_panic(buffer);
}
