#include "axys.h"

/* The compiler may turn loops into calls to these, so they must not be
 * "optimised" back into themselves. */
#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;

    while (n--) {
        *d++ = (u8)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a;
    const u8 *y = b;

    for (; n; --n, ++x, ++y) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        ++n;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (u8)*a - (u8)*b;
}

void puts(const char *s)
{
    write(1, s, strlen(s));
}

void put_u64(u64 value)
{
    char buf[24];
    int i = 23;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    puts(&buf[i]);
}

void put_hex(u64 value)
{
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        buf[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    }
    buf[18] = '\0';
    puts(buf);
}
