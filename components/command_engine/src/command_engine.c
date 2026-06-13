#include "command_engine.h"

#include "esp_log.h"

static const char *TAG = "command_engine";

esp_err_t command_engine_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}
