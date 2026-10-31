#ifndef VP_TIME_CALENDAR_H
#define VP_TIME_CALENDAR_H

#include <stdint.h>
#include <string.h>

#define VP_TIME_OVERFLOW ((201u << 9) | 116u)
#define VP_TIME_INVALID_ARGUMENT ((901u << 9) | 116u)

/* Gregorian years use astronomical numbering (year zero is a leap year).
 * Floor division is necessary for dates before the Unix epoch. */
static inline int64_t vp_time_floor_div(int64_t n, int64_t d) {
    int64_t q = n / d;
    return q - (n % d < 0);
}

static inline int64_t vp_time_year_days(int32_t year) {
    return 365ll * year + vp_time_floor_div((int64_t)year + 3, 4)
        - vp_time_floor_div((int64_t)year + 99, 100)
        + vp_time_floor_div((int64_t)year + 399, 400);
}

/* Locked SDK wire layout: CalendarTime (8 bytes) at offset 0, followed by
 * sf::CalendarAdditionalInfo (24 bytes) at offset 8. No host timezone/DST or
 * time_t width is involved in this explicit UTC-only service. */
static inline uint32_t vp_time_calendar_utc(int64_t seconds, uint32_t words[8]) {
    static const unsigned char month_days[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = vp_time_floor_div(seconds, 86400);
    int64_t absolute = days + 719528; /* Gregorian 0000-01-01 -> 1970-01-01 */
    int32_t low = -32768, high = 32768;
    int32_t year, yday, remaining, month = 0, leap, weekday;
    uint32_t within_day;
    unsigned char wire[32] = {0};
    if (absolute < vp_time_year_days(low) ||
        absolute >= vp_time_year_days(high)) return VP_TIME_OVERFLOW;
    while (high - low > 1) {
        int32_t mid = low + (high - low) / 2;
        if (vp_time_year_days(mid) <= absolute) low = mid;
        else high = mid;
    }
    year = low;
    yday = (int32_t)(absolute - vp_time_year_days(year));
    leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    remaining = yday;
    while (month < 11) {
        int32_t count = month_days[month] + (month == 1 && leap);
        if (remaining < count) break;
        remaining -= count;
        ++month;
    }
    /* Using the remainder avoids days*86400 overflowing for INT64_MIN. */
    within_day = (uint32_t)((seconds % 86400 + 86400) % 86400);
    weekday = (int32_t)((days + 4) % 7);
    if (weekday < 0) weekday += 7;
    wire[0] = (unsigned char)year;
    wire[1] = (unsigned char)((uint16_t)year >> 8);
    wire[2] = (unsigned char)(month + 1);
    wire[3] = (unsigned char)(remaining + 1);
    wire[4] = (unsigned char)(within_day / 3600u);
    wire[5] = (unsigned char)(within_day / 60u % 60u);
    wire[6] = (unsigned char)(within_day % 60u);
    wire[8] = (unsigned char)weekday;
    wire[12] = (unsigned char)yday;
    wire[13] = (unsigned char)(yday >> 8);
    memcpy(wire + 16, "UTC", 3);
    memcpy(words, wire, sizeof(wire));
    return 0;
}

#endif
