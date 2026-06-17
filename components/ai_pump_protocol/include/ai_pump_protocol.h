#ifndef HYDRA_AI_PUMP_PROTOCOL_H
#define HYDRA_AI_PUMP_PROTOCOL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "pump_registry.h"

#define AI_PUMP_ATTR_LIVE_DEMO_SCENE_NERO 0x0194
#define AI_PUMP_PRIMITIVE_PUMP_V1         0x02
#define AI_PUMP_SCENE_ID                  1
#define AI_PUMP_PRIMITIVE_BYTES           13
#define AI_PUMP_SCENE_NAME_BYTES          16
#define AI_PUMP_LIVE_DEMO_SCENE_BYTES     (4 + 2 + 2 + AI_PUMP_SCENE_NAME_BYTES + AI_PUMP_PRIMITIVE_BYTES)
#define AI_PUMP_SET_LDS_PAYLOAD_BYTES     (5 + AI_PUMP_LIVE_DEMO_SCENE_BYTES)

typedef struct {
    pump_mode_t mode;
    uint8_t     speed_percent;      /* MaxSpeed, 0..100 */
    uint8_t     min_speed_percent;  /* Random MinSpeed, 0..100 */
    uint8_t     variance_percent;   /* Random Variance, 0..100 */
    uint32_t    on_time_ms;         /* Pulse OnTime */
    uint32_t    off_time_ms;        /* Pulse OffTime */
    uint32_t    pulse_time_ms;      /* ShortPulse/Gyre Time */
    uint16_t    start_time_ms;      /* ExpandingPulse StartTime */
    uint16_t    end_time_ms;        /* ExpandingPulse EndTime */
    uint16_t    phase_shift_deg;    /* Sync/EcoSmartBack PhaseShift */
    bool        has_master;
    uint8_t     master[8];          /* Mobius IPv6 last8() value */
} ai_pump_command_t;

bool ai_pump_mode_is_supported(uint8_t mode);
bool ai_pump_mode_is_orbit_supported(uint8_t mode);

size_t ai_pump_build_live_demo_scene_nero_write(const ai_pump_command_t *cmd,
                                                uint16_t timeout_seconds,
                                                uint8_t *out,
                                                size_t out_cap);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t ai_pump_protocol_init(void);
#endif

#endif /* HYDRA_AI_PUMP_PROTOCOL_H */
