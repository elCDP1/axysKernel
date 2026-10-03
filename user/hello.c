#include "axys.h"

int main(const char *args, size_t len)
{
    u8 rnd[8];

    (void)len;
    puts("hello from ring 3, pid ");
    put_u64((u64)getpid());
    puts(args != 0 ? ", args=\"" : "\n");
    if (args != 0) {
        puts(args);
        puts("\"\n");
    }
    if (getrandom(rnd, sizeof(rnd)) == (long)sizeof(rnd)) {
        u64 v = 0;

        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | rnd[i];
        }
        puts("random: ");
        put_hex(v);
        puts("\n");
    }
    return 42;
}
