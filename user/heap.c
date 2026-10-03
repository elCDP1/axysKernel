#include "axys.h"

/* sbrk + memory integrity: grow the heap, fill it, verify it. */
int main(const char *args, size_t len)
{
    (void)args;
    (void)len;
    u8 *a = sbrk(100000);

    if ((long)a < 0) {
        return 1;
    }
    for (size_t i = 0; i < 100000; ++i) {
        a[i] = (u8)(i * 7);
    }
    for (size_t i = 0; i < 100000; ++i) {
        if (a[i] != (u8)(i * 7)) {
            return 2;
        }
    }
    u8 *b = sbrk(4096);
    if ((long)b != (long)a + 100000) {
        return 3;
    }
    if ((long)sbrk(1L << 40) >= 0) { /* an absurd request must be refused */
        return 4;
    }
    return 0;
}
