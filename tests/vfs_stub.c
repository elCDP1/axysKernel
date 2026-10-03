#include <stdlib.h>
#include "axys/console.h"
#include "axys/panic.h"

void axys_console_write(const char *text) { (void)text; }
void axys_console_write_len(const char *text, size_t length) { (void)text; (void)length; }
void axys_panic(const char *message) { (void)message; abort(); }

#include "axys/heap.h"
void *axys_kmalloc(size_t size) { return malloc(size); }
void *axys_kzalloc(size_t size) { return calloc(1, size); }
void axys_kfree(void *pointer) { free(pointer); }
