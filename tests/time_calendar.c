/* Exercise the actual UTC converter and exact SDK reply bytes. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include "time_calendar.h"

static void check(int64_t timestamp, int year, int month, int day,
                  int hour, int minute, int second, int weekday, int yday) {
    uint32_t words[8];
    const unsigned char *b = (const unsigned char *)words;
    assert(vp_time_calendar_utc(timestamp, words) == 0);
    assert((int16_t)(b[0] | ((uint16_t)b[1] << 8)) == year);
    assert(b[2] == month && b[3] == day);
    assert(b[4] == hour && b[5] == minute && b[6] == second && b[7] == 0);
    assert(words[2] == (uint32_t)weekday && words[3] == (uint32_t)yday);
    assert(memcmp(b + 16, "UTC\0\0\0\0\0", 8) == 0);
    assert(words[6] == 0 && words[7] == 0);
}

int main(void) {
    uint32_t words[8];
    check(0, 1970,1,1,0,0,0,4,0);
    check(-1, 1969,12,31,23,59,59,3,364);
    check(-2203891200ll, 1900,3,1,0,0,0,4,59); /* century not leap */
    check(951827696ll, 2000,2,29,12,34,56,2,59); /* 400-year leap */
    check(1709251199ll, 2024,2,29,23,59,59,4,59);
    check(1792627200ll, 2026,10,22,0,0,0,4,294);
    check(2147483648ll, 2038,1,19,3,14,8,2,18);
    check(4107542400ll, 2100,3,1,0,0,0,1,59);
    assert(vp_time_calendar_utc(INT64_MIN, words) == VP_TIME_OVERFLOW);
    assert(vp_time_calendar_utc(INT64_MAX, words) == VP_TIME_OVERFLOW);
    /* Independent host libc reference over years 1..9999, including dates
     * before 1970 and after the 32-bit time_t boundary. */
    for (int64_t t = -62135596800ll; t < 253402300800ll; t += 217456789ll) {
        time_t host_time = (time_t)t;
        struct tm *reference = gmtime(&host_time);
        /* Some host C libraries reject timestamps outside their supported
         * range. The fixed checks above already cover the ABI boundaries; the
         * loop only adds independent reference dates where the host can
         * provide one. */
        if (!reference) continue;
        check(t, reference->tm_year + 1900, reference->tm_mon + 1,
              reference->tm_mday, reference->tm_hour, reference->tm_min,
              reference->tm_sec, reference->tm_wday, reference->tm_yday);
    }
    puts("UTC calendar ABI and date regression passed");
    return 0;
}
