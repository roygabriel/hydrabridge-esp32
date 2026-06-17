#include "ai_pump_protocol.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "ai_pump_protocol";
#endif

static inline void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t pct_to_tenths(uint8_t pct)
{
    return (uint16_t)pct * 10;
}

bool ai_pump_mode_is_supported(uint8_t mode)
{
    switch ((pump_mode_t)mode) {
        case PUMP_MODE_CONSTANT:
        case PUMP_MODE_LAGOON:
        case PUMP_MODE_REEF_CREST:
        case PUMP_MODE_NUTRIENT_TRANSPORT:
        case PUMP_MODE_TIDAL_SWELL:
        case PUMP_MODE_SHORT_PULSE:
        case PUMP_MODE_GYRE:
        case PUMP_MODE_TRANSITION:
        case PUMP_MODE_EXPANDING_PULSE:
        case PUMP_MODE_SYNC:
        case PUMP_MODE_ECOSMART_BACK:
        case PUMP_MODE_FEED:
        case PUMP_MODE_BATTERY_BACKUP:
        case PUMP_MODE_RANDOM:
        case PUMP_MODE_PULSE:
            return true;
        default:
            return false;
    }
}

bool ai_pump_mode_is_orbit_supported(uint8_t mode)
{
    switch ((pump_mode_t)mode) {
        case PUMP_MODE_CONSTANT:
        case PUMP_MODE_RANDOM:
        case PUMP_MODE_PULSE:
        case PUMP_MODE_SYNC:
        case PUMP_MODE_FEED:
            return true;
        default:
            return false;
    }
}

static bool build_primitive(const ai_pump_command_t *cmd, uint8_t out[AI_PUMP_PRIMITIVE_BYTES])
{
    if (!cmd || !out) return false;
    if (cmd->speed_percent > 100 ||
        cmd->min_speed_percent > 100 ||
        cmd->variance_percent > 100) return false;
    if (!ai_pump_mode_is_supported((uint8_t)cmd->mode)) return false;

    memset(out, 0, AI_PUMP_PRIMITIVE_BYTES);
    out[0] = (uint8_t)cmd->mode;

    switch (cmd->mode) {
        case PUMP_MODE_CONSTANT:
        case PUMP_MODE_LAGOON:
        case PUMP_MODE_REEF_CREST:
        case PUMP_MODE_NUTRIENT_TRANSPORT:
        case PUMP_MODE_TIDAL_SWELL:
        case PUMP_MODE_FEED:
        case PUMP_MODE_BATTERY_BACKUP:
            put_u16_le(&out[1], pct_to_tenths(cmd->speed_percent));
            return true;

        case PUMP_MODE_RANDOM:
            put_u16_le(&out[1], pct_to_tenths(cmd->min_speed_percent));
            put_u16_le(&out[3], pct_to_tenths(cmd->speed_percent));
            put_u16_le(&out[5], pct_to_tenths(cmd->variance_percent));
            return true;

        case PUMP_MODE_PULSE:
            if (cmd->on_time_ms == 0 || cmd->off_time_ms == 0) return false;
            put_u16_le(&out[1], pct_to_tenths(cmd->speed_percent));
            put_u32_le(&out[3], cmd->on_time_ms);
            put_u32_le(&out[7], cmd->off_time_ms);
            return true;

        case PUMP_MODE_SHORT_PULSE:
        case PUMP_MODE_GYRE:
            if (cmd->pulse_time_ms == 0) return false;
            put_u16_le(&out[1], pct_to_tenths(cmd->speed_percent));
            put_u32_le(&out[3], cmd->pulse_time_ms);
            return true;

        case PUMP_MODE_EXPANDING_PULSE:
            put_u16_le(&out[1], pct_to_tenths(cmd->speed_percent));
            put_u16_le(&out[3], cmd->start_time_ms);
            put_u16_le(&out[5], cmd->end_time_ms);
            return true;

        case PUMP_MODE_SYNC:
        case PUMP_MODE_ECOSMART_BACK:
            if (!cmd->has_master) return false;
            put_u16_le(&out[1], pct_to_tenths(cmd->speed_percent));
            put_u16_le(&out[3], cmd->phase_shift_deg);
            memcpy(&out[5], cmd->master, 8);
            return true;

        case PUMP_MODE_TRANSITION:
            out[1] = 0; /* Linear ramp type. */
            return true;

        default:
            return false;
    }
}

size_t ai_pump_build_live_demo_scene_nero_write(const ai_pump_command_t *cmd,
                                                uint16_t timeout_seconds,
                                                uint8_t *out,
                                                size_t out_cap)
{
    if (!cmd || !out) return 0;
    if (out_cap < AI_PUMP_SET_LDS_PAYLOAD_BYTES) return 0;
    uint8_t primitive[AI_PUMP_PRIMITIVE_BYTES];
    if (!build_primitive(cmd, primitive)) return 0;

    put_u16_le(&out[0], AI_PUMP_ATTR_LIVE_DEMO_SCENE_NERO);
    out[2] = 0;
    out[3] = 1;
    out[4] = AI_PUMP_LIVE_DEMO_SCENE_BYTES;

    uint8_t *lds = &out[5];
    size_t k = 0;
    lds[k++] = AI_PUMP_PRIMITIVE_PUMP_V1;
    lds[k++] = 0;
    lds[k++] = 0;
    lds[k++] = 0;

    put_u16_le(&lds[k], AI_PUMP_SCENE_ID); k += 2;
    put_u16_le(&lds[k], timeout_seconds); k += 2;
    memset(&lds[k], 0, AI_PUMP_SCENE_NAME_BYTES);
    k += AI_PUMP_SCENE_NAME_BYTES;

    memcpy(&lds[k], primitive, sizeof primitive);

    return AI_PUMP_SET_LDS_PAYLOAD_BYTES;
}

#ifdef ESP_PLATFORM
esp_err_t ai_pump_protocol_init(void)
{
    ESP_LOGI(TAG, "init (LiveDemoSceneNero attr=0x%04x, %u bytes)",
             AI_PUMP_ATTR_LIVE_DEMO_SCENE_NERO,
             (unsigned)AI_PUMP_LIVE_DEMO_SCENE_BYTES);
    return ESP_OK;
}
#endif
