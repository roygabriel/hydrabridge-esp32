#include "fsci_codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "fsci_codec";
#endif

/* CRC-16/CCITT-FALSE bit-by-bit. The packet sizes we deal with are tiny
 * (~70 bytes per write, even smaller per confirm) so the loop cost is
 * negligible compared to BLE write latency. A table-based version can
 * replace this later without API changes if profiling demands it. */
uint16_t fsci_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    if (!data) return crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= ((uint16_t)data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

#ifdef ESP_PLATFORM
esp_err_t fsci_codec_init(void)
{
    ESP_LOGI(TAG, "init (poly=0x1021 init=0xFFFF)");
    return ESP_OK;
}
#endif
