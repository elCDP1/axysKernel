#ifndef AXYS_VGA_H
#define AXYS_VGA_H

#include "axys/types.h"

void axys_vga_init(void);
void axys_vga_clear(void);
void axys_vga_putc(char character);
void axys_vga_newline(void);
/* Push the hardware cursor if buffered writes left it stale. Cheap to call
 * opportunistically (e.g. at panic time) so the cursor is never visibly "lost"
 * mid-line for longer than necessary. */
void axys_vga_flush(void);

#endif
