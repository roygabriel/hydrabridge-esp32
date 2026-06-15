#include "sun_service.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
static const char *TAG = "sun_service";
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double deg_to_rad(double deg) { return deg * (M_PI / 180.0); }
static double rad_to_deg(double rad) { return rad * (180.0 / M_PI); }

static double normalize_degrees(double v)
{
    while (v < 0.0) v += 360.0;
    while (v >= 360.0) v -= 360.0;
    return v;
}

static double normalize_hours(double v)
{
    while (v < 0.0) v += 24.0;
    while (v >= 24.0) v -= 24.0;
    return v;
}

static int day_of_year(int year, int month, int day)
{
    static const int days_before_month[] =
        { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    bool leap = ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
    return days_before_month[month] + day + (leap && month > 2 ? 1 : 0);
}

static bool sun_event_utc_hour(int year, int month, int day,
                               double lat, double lon, bool sunrise,
                               double *out_hour)
{
    const double zenith = 90.833;
    int n = day_of_year(year, month, day);
    double lng_hour = lon / 15.0;
    double t = (double)n + (((sunrise ? 6.0 : 18.0) - lng_hour) / 24.0);
    double m = (0.9856 * t) - 3.289;
    double l = normalize_degrees(m + (1.916 * sin(deg_to_rad(m))) +
                                 (0.020 * sin(deg_to_rad(2.0 * m))) + 282.634);
    double ra = rad_to_deg(atan(0.91764 * tan(deg_to_rad(l))));
    ra = normalize_degrees(ra);

    double l_quadrant = floor(l / 90.0) * 90.0;
    double ra_quadrant = floor(ra / 90.0) * 90.0;
    ra = (ra + (l_quadrant - ra_quadrant)) / 15.0;

    double sin_dec = 0.39782 * sin(deg_to_rad(l));
    double cos_dec = cos(asin(sin_dec));
    double cos_h = (cos(deg_to_rad(zenith)) - (sin_dec * sin(deg_to_rad(lat)))) /
                   (cos_dec * cos(deg_to_rad(lat)));
    if (cos_h > 1.0 || cos_h < -1.0) return false;

    double h = sunrise ? (360.0 - rad_to_deg(acos(cos_h))) : rad_to_deg(acos(cos_h));
    h /= 15.0;
    double local_mean = h + ra - (0.06571 * t) - 6.622;
    *out_hour = normalize_hours(local_mean - lng_hour);
    return true;
}

bool sun_calc_utc_minutes(int year, int month, int day,
                          int32_t latitude_e7, int32_t longitude_e7,
                          int *sunrise_utc_minute,
                          int *sunset_utc_minute)
{
    if (!sunrise_utc_minute || !sunset_utc_minute) return false;
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    double lat = (double)latitude_e7 / 10000000.0;
    double lon = (double)longitude_e7 / 10000000.0;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;

    double sr_h = 0.0, ss_h = 0.0;
    if (!sun_event_utc_hour(year, month, day, lat, lon, true, &sr_h)) return false;
    if (!sun_event_utc_hour(year, month, day, lat, lon, false, &ss_h)) return false;
    *sunrise_utc_minute = (int)lround(sr_h * 60.0);
    *sunset_utc_minute = (int)lround(ss_h * 60.0);
    if (*sunrise_utc_minute >= 1440) *sunrise_utc_minute -= 1440;
    if (*sunset_utc_minute >= 1440) *sunset_utc_minute -= 1440;
    return true;
}

#ifdef ESP_PLATFORM

static config_sun_t s_cfg;

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static time_t utc_epoch_from_ymd(int year, int month, int day)
{
    return (time_t)(days_from_civil(year, (unsigned)month, (unsigned)day) * 86400LL);
}

static bool clock_is_valid(time_t now)
{
    return now > 1704067200; /* 2024-01-01 */
}

static void fill_time_text(int minute, char out[16])
{
    if (!out) return;
    if (minute < 0) {
        strncpy(out, "--:--", 15);
        out[15] = '\0';
        return;
    }
    snprintf(out, 16, "%02d:%02d", minute / 60, minute % 60);
}

static bool compute_today(sun_service_status_t *out)
{
    if (!out) return false;
    time_t now = time(NULL);
    if (!clock_is_valid(now) || !s_cfg.enabled) return false;

    struct tm local_now;
    localtime_r(&now, &local_now);
    int year = local_now.tm_year + 1900;
    int month = local_now.tm_mon + 1;
    int day = local_now.tm_mday;

    int sr_utc = 0, ss_utc = 0;
    if (!sun_calc_utc_minutes(year, month, day, s_cfg.latitude_e7, s_cfg.longitude_e7,
                              &sr_utc, &ss_utc)) {
        return false;
    }

    time_t utc_midnight = utc_epoch_from_ymd(year, month, day);
    struct tm sr_local, ss_local;
    time_t sr_epoch = utc_midnight + (time_t)sr_utc * 60;
    time_t ss_epoch = utc_midnight + (time_t)ss_utc * 60;
    localtime_r(&sr_epoch, &sr_local);
    localtime_r(&ss_epoch, &ss_local);

    out->valid = true;
    out->year = year;
    out->month = month;
    out->day = day;
    out->sunrise_minute = sr_local.tm_hour * 60 + sr_local.tm_min;
    out->sunset_minute = ss_local.tm_hour * 60 + ss_local.tm_min;
    fill_time_text(out->sunrise_minute, out->sunrise_local);
    fill_time_text(out->sunset_minute, out->sunset_local);
    return true;
}

esp_err_t sun_service_init(void)
{
    config_store_load_sun(&s_cfg);
    ESP_LOGI(TAG, "init enabled=%d location=%s", s_cfg.enabled, s_cfg.location_label);
    return ESP_OK;
}

esp_err_t sun_service_reconfigure(void)
{
    config_store_load_sun(&s_cfg);
    ESP_LOGI(TAG, "reconfigure enabled=%d location=%s", s_cfg.enabled, s_cfg.location_label);
    return ESP_OK;
}

void sun_service_get_status(sun_service_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = s_cfg.enabled;
    out->sunrise_minute = -1;
    out->sunset_minute = -1;
    strncpy(out->location_label, s_cfg.location_label, sizeof out->location_label - 1);
    out->latitude_e7 = s_cfg.latitude_e7;
    out->longitude_e7 = s_cfg.longitude_e7;
    fill_time_text(-1, out->sunrise_local);
    fill_time_text(-1, out->sunset_local);
    compute_today(out);
}

bool sun_service_get_today(int *sunrise_minute, int *sunset_minute)
{
    sun_service_status_t st;
    sun_service_get_status(&st);
    if (!st.valid) return false;
    if (sunrise_minute) *sunrise_minute = st.sunrise_minute;
    if (sunset_minute) *sunset_minute = st.sunset_minute;
    return true;
}

#endif /* ESP_PLATFORM */
