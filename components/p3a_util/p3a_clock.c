// SPDX-License-Identifier: Apache-2.0
// See p3a_clock.h. No IDF includes: also compiled by the host tests.

#include "p3a_clock.h"

#include <stdatomic.h>
#include <stdio.h>

static atomic_bool s_set;

void p3a_clock_mark_set(void)
{
    atomic_store(&s_set, true);
}

bool p3a_clock_is_set(void)
{
    return atomic_load(&s_set);
}

esp_err_t p3a_clock_format_iso8601(time_t when, char *buf, size_t len)
{
    if (buf == NULL || len < P3A_CLOCK_ISO8601_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so
    // that this does not depend on the C library's gmtime_r or time zone.
    long long t = (long long)when;
    long long days = t / 86400;
    long long rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long day = doy - (153 * mp + 2) / 5 + 1;
    long long month = mp < 10 ? mp + 3 : mp - 9;
    long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    if (year < 0 || year > 9999) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", (int)year, (int)month, (int)day, (int)(rem / 3600),
             (int)(rem % 3600 / 60), (int)(rem % 60));
    return ESP_OK;
}

esp_err_t p3a_clock_now_iso8601(char *buf, size_t len)
{
    if (buf == NULL || len < P3A_CLOCK_ISO8601_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!p3a_clock_is_set()) {
        return ESP_ERR_INVALID_STATE;
    }
    return p3a_clock_format_iso8601(time(NULL), buf, len);
}
