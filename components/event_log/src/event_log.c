#include "event_log.h"

#include "esp_log.h"

static const char *TAG = "event_log";

esp_err_t event_log_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}
