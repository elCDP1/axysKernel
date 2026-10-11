#ifndef AXYS_TIMECONV_H
#define AXYS_TIMECONV_H

#include "axys/types.h"

/* Calendar arithmetic and CMOS RTC register decoding. Pure functions (no I/O),
 * so they are unit-tested on the host. Dates are proleptic Gregorian, UTC. */

struct axys_civil {
    axys_uint32_t year;    /* 1970.. */
    axys_uint32_t month;   /* 1..12 */
    axys_uint32_t day;     /* 1..31 */
    axys_uint32_t hour;    /* 0..23 */
    axys_uint32_t minute;  /* 0..59 */
    axys_uint32_t second;  /* 0..59 */
    axys_uint32_t weekday; /* 0 = Sunday (filled by axys_civil_from_unix) */
};

/* Seconds since 1970-01-01 00:00:00, or -1 when any field is out of range
 * (including 29 February in a non-leap year) or the year is before 1970. */
axys_int64_t axys_unix_from_civil(const struct axys_civil *civil);

/* Break `seconds` (>= 0) into calendar fields. */
void axys_civil_from_unix(axys_int64_t seconds, struct axys_civil *civil);

/* The CMOS RTC registers as read from the hardware. */
struct axys_rtc_raw {
    axys_uint8_t second, minute, hour, day, month, year, century;
};

/* Decode raw RTC registers. `status_b` is register 0x0B: bit 1 = 24-hour mode,
 * bit 2 = binary (not BCD) values. In 12-hour mode bit 7 of the hour register
 * is PM. `century` of 0 (or one outside 19..21 once decoded) means "not
 * provided": two-digit years then map 70..99 -> 19xx and 00..69 -> 20xx.
 * Returns 0, or -1 for impossible values (an absent RTC reads as 0xff). */
int axys_rtc_decode(const struct axys_rtc_raw *raw, axys_uint8_t status_b, struct axys_civil *civil);

#endif
