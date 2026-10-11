#ifndef AXYS_RTC_H
#define AXYS_RTC_H

#include "axys/timeconv.h"
#include "axys/types.h"

/* CMOS real-time clock (ports 0x70/0x71). The RTC keeps UTC wall time when the
 * firmware or hypervisor sets it so. */

/* Read the hardware clock. Returns 0, or -1 when no plausible RTC answers. */
int axys_rtc_read(struct axys_civil *out);

/* Current time in seconds since 1970 (hardware clock plus the software offset
 * set by axys_time_set), or -1 without an RTC. */
axys_int64_t axys_time_now(void);

/* Make axys_time_now() return `seconds` from now on. The hardware clock is not
 * written: this is a software offset that lasts until reboot. Returns 0 or -1. */
int axys_time_set(axys_int64_t seconds);

#endif
