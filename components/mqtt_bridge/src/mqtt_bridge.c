#include "mqtt_bridge.h"

#include "esp_log.h"

static const char *TAG = "mqtt_bridge";

esp_err_t mqtt_bridge_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}
