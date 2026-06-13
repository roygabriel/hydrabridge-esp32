/* Phase 4.3: ESP-Modbus driver wiring.
 *
 * Configures UART in RS485 half-duplex mode, registers the holding-
 * register area pointing at modbus_interface's store, and starts a
 * FreeRTOS task that pumps the modbus stack's event loop. Whenever
 * the master writes to a holding register the task calls
 * modbus_store_process_pending() so the snapshot-on-command_seq
 * dispatch fires immediately. A second tick task refreshes the
 * status mirrors at 4Hz.
 *
 * ESP-IDF only. */

#ifdef ESP_PLATFORM

#include "modbus_interface.h"
#include "modbus_registers.h"
#include "config_store.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_modbus_slave.h"
#include "driver/uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mb_slave";

static TaskHandle_t s_slave_task = NULL;
static TaskHandle_t s_refresh_task = NULL;
static void *s_mbc_handle = NULL;

static void slave_event_loop(void *arg)
{
    (void)arg;
    const mb_event_group_t mask = (mb_event_group_t)(
        MB_EVENT_HOLDING_REG_WR |
        MB_EVENT_HOLDING_REG_RD |
        MB_EVENT_INPUT_REG_RD   |
        MB_EVENT_COILS_WR       |
        MB_EVENT_COILS_RD       |
        MB_EVENT_DISCRETE_RD);

    ESP_LOGI(TAG, "event loop started");
    for (;;) {
        mb_event_group_t evt = mbc_slave_check_event(mask);
        if (evt & MB_EVENT_HOLDING_REG_WR) {
            modbus_store_process_pending();
        }
    }
}

static void refresh_loop(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "status-mirror refresh task started (4Hz)");
    for (;;) {
        modbus_store_refresh_status_mirrors();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static uart_parity_t parity_from_config(modbus_parity_t p)
{
    switch (p) {
        case MODBUS_PARITY_EVEN: return UART_PARITY_EVEN;
        case MODBUS_PARITY_ODD:  return UART_PARITY_ODD;
        case MODBUS_PARITY_NONE:
        default:                 return UART_PARITY_DISABLE;
    }
}

esp_err_t modbus_slave_driver_start(void)
{
    config_modbus_t mb;
    config_store_load_modbus(&mb);

    if (!mb.enabled) {
        ESP_LOGW(TAG, "modbus disabled in config; not starting slave");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "starting RS485 slave: addr=%u baud=%lu uart=%u tx=%d rx=%d de=%d parity=%d",
             (unsigned)mb.slave_address, (unsigned long)mb.baud_rate,
             (unsigned)mb.uart_port, (int)mb.tx_pin, (int)mb.rx_pin,
             (int)mb.rts_de_pin, (int)mb.parity);

    esp_err_t err = mbc_slave_init(MB_PORT_SERIAL_SLAVE, &s_mbc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_init failed: 0x%x", err);
        return err;
    }

    mb_communication_info_t comm = {0};
    comm.mode       = MB_MODE_RTU;
    comm.slave_addr = mb.slave_address;
    comm.port       = (uart_port_t)mb.uart_port;
    comm.baudrate   = mb.baud_rate;
    comm.parity     = parity_from_config(mb.parity);
    err = mbc_slave_setup((void *)&comm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_setup failed: 0x%x", err);
        return err;
    }

    mb_register_area_descriptor_t reg_area = {0};
    reg_area.type         = MB_PARAM_HOLDING;
    reg_area.start_offset = 0;
    reg_area.address      = modbus_store_raw_array();
    reg_area.size         = MODBUS_STORE_REG_COUNT * sizeof(uint16_t);
    err = mbc_slave_set_descriptor(reg_area);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_set_descriptor failed: 0x%x", err);
        return err;
    }

    err = mbc_slave_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mbc_slave_start failed: 0x%x", err);
        return err;
    }

    err = uart_set_pin((uart_port_t)mb.uart_port,
                       mb.tx_pin, mb.rx_pin, mb.rts_de_pin,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: 0x%x", err);
        return err;
    }
    err = uart_set_mode((uart_port_t)mb.uart_port, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_mode RS485 failed: 0x%x", err);
        return err;
    }

    BaseType_t b = xTaskCreate(slave_event_loop, "mb_slave_evt", 4096, NULL, 6, &s_slave_task);
    if (b != pdPASS) return ESP_ERR_NO_MEM;
    b = xTaskCreate(refresh_loop, "mb_refresh", 3072, NULL, 4, &s_refresh_task);
    if (b != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "RS485 slave ready");
    return ESP_OK;
}

#endif /* ESP_PLATFORM */
