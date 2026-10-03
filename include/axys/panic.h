#ifndef AXYS_PANIC_H
#define AXYS_PANIC_H

#include "axys/types.h"

AXYS_NORETURN void axys_panic(const char *message);
AXYS_NORETURN void axys_panic_format(const char *format, ...);

#endif
