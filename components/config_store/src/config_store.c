#include "config_store.h"

#include "esp_log.h"

static const char *TAG = "config_store";

esp_err_t config_store_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}
