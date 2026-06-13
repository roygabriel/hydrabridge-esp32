#include "modbus_interface.h"

#include "esp_log.h"

static const char *TAG = "modbus_interface";

esp_err_t modbus_interface_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}
