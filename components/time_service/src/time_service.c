#ifdef ESP_PLATFORM

#include "time_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"

static const char *TAG = "time_service";

static config_time_t s_cfg;
static bool s_started;
static bool s_synced;
static int64_t s_last_sync_epoch;

static void format_local(time_t t, char out[32])
{
    if (!out) return;
    if (t <= 0) {
        strncpy(out, "unsynced", 31);
        out[31] = '\0';
        return;
    }
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void apply_timezone(const char *tz)
{
    const char *use_tz = (tz && tz[0]) ? tz : "UTC0";
    setenv("TZ", use_tz, 1);
    tzset();
}

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    s_last_sync_epoch = tv ? (int64_t)tv->tv_sec : (int64_t)time(NULL);
    ESP_LOGI(TAG, "time synchronized epoch=%lld", (long long)s_last_sync_epoch);
}

static esp_err_t start_sntp_locked(void)
{
    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "time sync disabled");
        return ESP_OK;
    }

    const char *server = s_cfg.server[0] ? s_cfg.server : "time.nist.gov";
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.wait_for_sync = false;
    config.start = true;
    config.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        esp_netif_sntp_deinit();
        err = esp_netif_sntp_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: 0x%x", err);
        return err;
    }
    ESP_LOGI(TAG, "time sync enabled server=%s tz=%s", server, s_cfg.timezone);
    return ESP_OK;
}

esp_err_t time_service_init(void)
{
    config_store_load_time(&s_cfg);
    apply_timezone(s_cfg.timezone);
    s_started = true;
    return start_sntp_locked();
}

esp_err_t time_service_reconfigure(void)
{
    config_store_load_time(&s_cfg);
    apply_timezone(s_cfg.timezone);
    s_synced = false;
    s_last_sync_epoch = 0;
    if (s_started) {
        esp_netif_sntp_deinit();
    } else {
        s_started = true;
    }
    return start_sntp_locked();
}

void time_service_get_status(time_service_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->enabled = s_cfg.enabled;
    out->synced = s_synced;
    strncpy(out->server, s_cfg.server, sizeof out->server - 1);
    strncpy(out->timezone, s_cfg.timezone, sizeof out->timezone - 1);
    time_t now = time(NULL);
    out->current_epoch = (int64_t)now;
    out->last_sync_epoch = s_last_sync_epoch;
    format_local(now, out->current_local);
    format_local((time_t)s_last_sync_epoch, out->last_sync_local);
}

bool time_service_is_synced(void)
{
    return s_synced;
}

time_t time_service_now(void)
{
    return time(NULL);
}

#endif /* ESP_PLATFORM */
