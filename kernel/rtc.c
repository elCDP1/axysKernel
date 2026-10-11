#include "axys/rtc.h"
#include "axys/cpu.h"
#include "axys/io.h"

#define CMOS_INDEX 0x70u
#define CMOS_DATA 0x71u

static axys_int64_t time_offset;

/* The index/data pair must not be interleaved with another reader: a task
 * switch between the two port accesses would return the wrong register. */
static axys_uint8_t cmos_read(axys_uint8_t reg)
{
    axys_uint64_t flags = axys_cpu_save_flags();
    axys_uint8_t value;

    axys_cpu_disable_interrupts();
    axys_outb(CMOS_INDEX, reg);
    value = axys_inb(CMOS_DATA);
    axys_cpu_restore_flags(flags);
    return value;
}

static int same(const struct axys_rtc_raw *a, const struct axys_rtc_raw *b)
{
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year && a->century == b->century;
}

/* Registers change once a second; the update-in-progress flag marks the moment
 * they are not coherent. Wait it out, then read twice and require agreement. */
static int snapshot(struct axys_rtc_raw *raw, axys_uint8_t *status_b)
{
    for (int spin = 0; spin < 100000; ++spin) {
        if ((cmos_read(0x0a) & 0x80u) == 0u) {
            raw->second = cmos_read(0x00);
            raw->minute = cmos_read(0x02);
            raw->hour = cmos_read(0x04);
            raw->day = cmos_read(0x07);
            raw->month = cmos_read(0x08);
            raw->year = cmos_read(0x09);
            raw->century = cmos_read(0x32);
            *status_b = cmos_read(0x0b);
            return 0;
        }
    }
    return -1;
}

int axys_rtc_read(struct axys_civil *out)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        struct axys_rtc_raw a, b;
        axys_uint8_t status_a, status_b;

        if (snapshot(&a, &status_a) != 0 || snapshot(&b, &status_b) != 0) {
            return -1;
        }
        if (status_a == status_b && same(&a, &b)) {
            return axys_rtc_decode(&a, status_a, out);
        }
    }
    return -1;
}

axys_int64_t axys_time_now(void)
{
    struct axys_civil civil;
    axys_int64_t base;

    if (axys_rtc_read(&civil) != 0) {
        return -1;
    }
    base = axys_unix_from_civil(&civil);
    return base < 0 ? -1 : base + time_offset;
}

int axys_time_set(axys_int64_t seconds)
{
    struct axys_civil civil;
    axys_int64_t base;

    if (seconds < 0 || axys_rtc_read(&civil) != 0) {
        return -1;
    }
    base = axys_unix_from_civil(&civil);
    if (base < 0) {
        return -1;
    }
    time_offset = seconds - base;
    return 0;
}
