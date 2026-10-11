#include "axys/console.h"
#include "axys/cpu.h"
#include "axys/serial.h"
#include "axys/vga.h"

/* Kernel log for `dmesg`: what the *kernel* printed (axys_printf), not what
 * user programs wrote to the console. Retaining both made the boot messages
 * vanish after the first few shell commands. A monotonic counter plus a
 * power-of-two ring: readers derive (start, length) from the counter alone, so
 * no head/tail pair can disagree. Pushes bracket with save/disable/restore
 * because the kernel prints from IRQ handlers too. 32 KiB holds a full boot. */
#define CONSOLE_LOG_BYTES 32768u

static char log_ring[CONSOLE_LOG_BYTES];
static axys_uint64_t log_count;

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

void axys_console_clear(void)
{
#if CONFIG_AXYS_CONSOLE_VGA
    axys_vga_clear();
#endif
#if CONFIG_AXYS_CONSOLE_SERIAL
    axys_serial_write("\x1b[2J\x1b[H", 7);
#endif
}

void axys_klog_append(const char *text, axys_size_t length)
{
    axys_uint64_t flags = axys_cpu_save_flags();

    axys_cpu_disable_interrupts();
    for (axys_size_t i = 0; i < length; ++i) {
        log_ring[log_count & (CONSOLE_LOG_BYTES - 1u)] = text[i];
        ++log_count;
    }
    axys_cpu_restore_flags(flags);
}

/* Copy up to `cap` bytes of the retained log into `out`, oldest first,
 * skipping the first `skip` retained bytes. Returns the bytes copied.
 * A concurrent putc can only append past our window (indices come from one
 * counter read), so the worst case is a slightly stale tail, never garbage. */
axys_size_t axys_console_log_copy(char *out, axys_size_t cap, axys_size_t skip)
{
    axys_uint64_t flags;
    axys_uint64_t count;
    axys_uint64_t kept;
    axys_uint64_t start;
    axys_size_t n = 0;

    if (out == AXYS_NULL || cap == 0) {
        return 0;
    }
    flags = axys_cpu_save_flags();
    axys_cpu_disable_interrupts();
    count = log_count;
    axys_cpu_restore_flags(flags);
    kept = count < CONSOLE_LOG_BYTES ? count : CONSOLE_LOG_BYTES;
    if (skip >= kept) {
        return 0;
    }
    start = (count - kept + skip) & (CONSOLE_LOG_BYTES - 1u);
    kept -= skip;
    if (kept > cap) {
        kept = cap;
    }
    for (axys_size_t i = 0; i < kept; ++i) {
        out[i] = log_ring[(start + i) & (CONSOLE_LOG_BYTES - 1u)];
    }
    n = kept;
    return n;
}
