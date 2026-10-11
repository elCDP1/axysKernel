#ifndef AXYS_CONSOLE_H
#define AXYS_CONSOLE_H

#include "axys/types.h"

void axys_console_init(void);
void axys_console_putc(char character);
void axys_console_write(const char *string);
void axys_console_write_len(const char *string, axys_size_t length);

/* Clear the screen and home the cursor on VGA, and send the ANSI equivalent
 * to the serial terminal. */
void axys_console_clear(void);

/* Append kernel-printed text to the log behind `dmesg` (called by axys_printf). */
void axys_klog_append(const char *text, axys_size_t length);

/* The retained kernel log (last 32 KiB), oldest first, behind `dmesg`. */
axys_size_t axys_console_log_copy(char *out, axys_size_t cap, axys_size_t skip);

#endif
