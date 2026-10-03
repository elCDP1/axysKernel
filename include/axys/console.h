#ifndef AXYS_CONSOLE_H
#define AXYS_CONSOLE_H

#include "axys/types.h"

void axys_console_init(void);
void axys_console_putc(char character);
void axys_console_write(const char *string);
void axys_console_write_len(const char *string, axys_size_t length);

#endif
