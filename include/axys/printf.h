#ifndef AXYS_PRINTF_H
#define AXYS_PRINTF_H

#include "axys/types.h"

typedef __builtin_va_list axys_va_list;

int axys_vsnprintf(char *buffer, axys_size_t size, const char *format, axys_va_list arguments);
int axys_snprintf(char *buffer, axys_size_t size, const char *format, ...);
int axys_printf(const char *format, ...);
int axys_vprintf(const char *format, axys_va_list arguments);

#endif
