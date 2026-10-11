#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "axys/timeconv.h"

static struct axys_civil mk(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    struct axys_civil c = {y, mo, d, h, mi, s, 0};
    return c;
}

int main(void)
{
    struct axys_civil c, d;

    /* known instants */
    c = mk(1970, 1, 1, 0, 0, 0);
    assert(axys_unix_from_civil(&c) == 0);
    axys_civil_from_unix(0, &d);
    assert(d.year == 1970 && d.month == 1 && d.day == 1 && d.weekday == 4); /* Thursday */
    c = mk(2000, 2, 29, 0, 0, 0);
    assert(axys_unix_from_civil(&c) == 951782400);
    c = mk(2023, 11, 14, 22, 13, 20);
    assert(axys_unix_from_civil(&c) == 1700000000);
    axys_civil_from_unix(1700000000, &d);
    assert(d.weekday == 2 && d.hour == 22 && d.minute == 13 && d.second == 20); /* Tuesday */
    c = mk(2038, 1, 19, 3, 14, 8); /* past the signed 32-bit rollover */
    assert(axys_unix_from_civil(&c) == 2147483648LL);

    /* invalid input is refused */
    c = mk(1969, 12, 31, 23, 59, 59); assert(axys_unix_from_civil(&c) == -1);
    c = mk(2023, 2, 29, 0, 0, 0);     assert(axys_unix_from_civil(&c) == -1); /* not a leap year */
    c = mk(2100, 2, 29, 0, 0, 0);     assert(axys_unix_from_civil(&c) == -1); /* century, not leap */
    c = mk(2000, 2, 29, 0, 0, 0);     assert(axys_unix_from_civil(&c) != -1); /* 400-year leap */
    c = mk(2024, 13, 1, 0, 0, 0);     assert(axys_unix_from_civil(&c) == -1);
    c = mk(2024, 0, 1, 0, 0, 0);      assert(axys_unix_from_civil(&c) == -1);
    c = mk(2024, 4, 31, 0, 0, 0);     assert(axys_unix_from_civil(&c) == -1);
    c = mk(2024, 1, 1, 24, 0, 0);     assert(axys_unix_from_civil(&c) == -1);
    c = mk(2024, 1, 1, 0, 60, 0);     assert(axys_unix_from_civil(&c) == -1);
    c = mk(2024, 1, 1, 0, 0, 60);     assert(axys_unix_from_civil(&c) == -1);
    assert(axys_unix_from_civil(NULL) == -1);

    /* every day from 1970 to 2200 round-trips and the weekday advances by one */
    {
        long long prev_weekday = -1;
        for (long long day = 0; day < 365LL * 230; ++day) {
            axys_civil_from_unix(day * 86400 + 12345, &d);
            assert(axys_unix_from_civil(&d) == day * 86400 + 12345);
            if (prev_weekday >= 0) assert(d.weekday == (unsigned)((prev_weekday + 1) % 7));
            prev_weekday = d.weekday;
        }
    }

    /* RTC decoding: BCD 24-hour, binary, 12-hour with PM, century handling */
    {
        struct axys_rtc_raw r = {0x45, 0x59, 0x23, 0x31, 0x12, 0x99, 0x19};
        assert(axys_rtc_decode(&r, 0x02, &d) == 0);       /* BCD, 24 h */
        assert(d.year == 1999 && d.month == 12 && d.day == 31 && d.hour == 23 && d.minute == 59 && d.second == 45);
        struct axys_rtc_raw b = {45, 59, 23, 31, 12, 25, 20};
        assert(axys_rtc_decode(&b, 0x06, &d) == 0 && d.year == 2025 && d.hour == 23); /* binary */
        struct axys_rtc_raw pm = {0x00, 0x30, 0x80 | 0x12, 0x01, 0x03, 0x24, 0x20};
        assert(axys_rtc_decode(&pm, 0x00, &d) == 0 && d.hour == 12 && d.year == 2024); /* 12 PM = noon */
        struct axys_rtc_raw am = {0x00, 0x30, 0x12, 0x01, 0x03, 0x24, 0x20};
        assert(axys_rtc_decode(&am, 0x00, &d) == 0 && d.hour == 0);                    /* 12 AM = midnight */
        struct axys_rtc_raw p1 = {0x00, 0x00, 0x80 | 0x01, 0x01, 0x03, 0x24, 0x20};
        assert(axys_rtc_decode(&p1, 0x00, &d) == 0 && d.hour == 13);
        struct axys_rtc_raw nc = {0x00, 0x00, 0x00, 0x01, 0x01, 0x69, 0x00};
        assert(axys_rtc_decode(&nc, 0x02, &d) == 0 && d.year == 2069);               /* no century register */
        struct axys_rtc_raw nc2 = {0x00, 0x00, 0x00, 0x01, 0x01, 0x70, 0x00};
        assert(axys_rtc_decode(&nc2, 0x02, &d) == 0 && d.year == 1970);
        struct axys_rtc_raw absent = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};      /* floating bus */
        assert(axys_rtc_decode(&absent, 0x02, &d) == -1);
        struct axys_rtc_raw badbcd = {0x6a, 0x00, 0x00, 0x01, 0x01, 0x24, 0x20};
        assert(axys_rtc_decode(&badbcd, 0x02, &d) == -1);
        struct axys_rtc_raw leap = {0, 0, 0, 0x29, 0x02, 0x23, 0x20};                 /* 2023-02-29 */
        assert(axys_rtc_decode(&leap, 0x02, &d) == -1);
        struct axys_rtc_raw h13 = {0, 0, 0x13, 1, 1, 0x24, 0x20};                      /* 13 in 12-hour mode */
        assert(axys_rtc_decode(&h13, 0x00, &d) == -1);
        assert(axys_rtc_decode(NULL, 0, &d) == -1);
    }
    puts("test_time: ok");
    return 0;
}
