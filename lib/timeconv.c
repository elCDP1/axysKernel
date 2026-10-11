#include "axys/timeconv.h"

static int is_leap(axys_uint32_t year)
{
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static axys_uint32_t days_in_month(axys_uint32_t year, axys_uint32_t month)
{
    static const axys_uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    return month == 2u && is_leap(year) ? 29u : days[month - 1u];
}

/* Days from 1970-01-01 to year-month-day (Howard Hinnant's algorithm). */
static axys_int64_t days_from_civil(axys_int64_t y, axys_uint32_t m, axys_uint32_t d)
{
    axys_int64_t era;
    axys_uint32_t yoe;
    axys_uint32_t doy;
    axys_uint32_t doe;

    y -= m <= 2u;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (axys_uint32_t)(y - era * 400);
    doy = (153u * (m + (m > 2u ? (axys_uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (axys_int64_t)doe - 719468;
}

axys_int64_t axys_unix_from_civil(const struct axys_civil *c)
{
    if (c == AXYS_NULL || c->year < 1970u || c->year > 9999u || c->month < 1u || c->month > 12u ||
        c->day < 1u || c->day > days_in_month(c->year, c->month) || c->hour > 23u || c->minute > 59u ||
        c->second > 59u) {
        return -1;
    }
    return days_from_civil(c->year, c->month, c->day) * 86400 + (axys_int64_t)c->hour * 3600 +
           (axys_int64_t)c->minute * 60 + (axys_int64_t)c->second;
}

void axys_civil_from_unix(axys_int64_t seconds, struct axys_civil *c)
{
    axys_int64_t days;
    axys_uint32_t rest;
    axys_int64_t z;
    axys_int64_t era;
    axys_uint32_t doe, yoe, doy, mp;
    axys_int64_t y;

    if (seconds < 0) {
        seconds = 0;
    }
    days = seconds / 86400;
    rest = (axys_uint32_t)(seconds % 86400);
    c->hour = rest / 3600u;
    c->minute = (rest % 3600u) / 60u;
    c->second = rest % 60u;
    c->weekday = (axys_uint32_t)((days + 4) % 7); /* 1970-01-01 was a Thursday */

    z = days + 719468;
    era = z / 146097;
    doe = (axys_uint32_t)(z - era * 146097);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    y = (axys_int64_t)yoe + era * 400;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    c->day = doy - (153u * mp + 2u) / 5u + 1u;
    c->month = mp < 10u ? mp + 3u : mp - 9u;
    c->year = (axys_uint32_t)(y + (c->month <= 2u));
}

static int bcd_ok(axys_uint8_t v)
{
    return (v & 0x0fu) <= 9u && (v >> 4) <= 9u;
}

static axys_uint32_t from_bcd(axys_uint8_t v)
{
    return (axys_uint32_t)((v >> 4) * 10u + (v & 0x0fu));
}

int axys_rtc_decode(const struct axys_rtc_raw *raw, axys_uint8_t status_b, struct axys_civil *out)
{
    int binary = (status_b & 0x04u) != 0u;
    int h24 = (status_b & 0x02u) != 0u;
    axys_uint8_t hour_reg;
    int pm = 0;
    axys_uint32_t sec, min, hour, day, mon, yy, cent;

    if (raw == AXYS_NULL || out == AXYS_NULL) {
        return -1;
    }
    hour_reg = raw->hour;
    if (!h24) {
        pm = (hour_reg & 0x80u) != 0u;
        hour_reg &= 0x7fu;
    }
    if (!binary &&
        (!bcd_ok(raw->second) || !bcd_ok(raw->minute) || !bcd_ok(hour_reg) || !bcd_ok(raw->day) ||
         !bcd_ok(raw->month) || !bcd_ok(raw->year))) {
        return -1;
    }
    sec = binary ? raw->second : from_bcd(raw->second);
    min = binary ? raw->minute : from_bcd(raw->minute);
    hour = binary ? hour_reg : from_bcd(hour_reg);
    day = binary ? raw->day : from_bcd(raw->day);
    mon = binary ? raw->month : from_bcd(raw->month);
    yy = binary ? raw->year : from_bcd(raw->year);
    if (!h24) {
        if (hour < 1u || hour > 12u) {
            return -1;
        }
        hour = hour % 12u + (pm ? 12u : 0u);
    }
    cent = raw->century == 0u ? 0u : (binary ? raw->century : (bcd_ok(raw->century) ? from_bcd(raw->century) : 0u));
    if (cent < 19u || cent > 21u) {
        cent = yy >= 70u ? 19u : 20u;
    }
    out->year = cent * 100u + yy;
    out->month = mon;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    out->weekday = 0;
    /* validate through the conversion (ranges, leap days) and fill the weekday */
    {
        axys_int64_t t = axys_unix_from_civil(out);

        if (t < 0) {
            return -1;
        }
        axys_civil_from_unix(t, out);
    }
    return 0;
}
