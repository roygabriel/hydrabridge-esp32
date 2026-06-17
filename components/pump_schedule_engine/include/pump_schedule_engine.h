#ifndef HYDRA_PUMP_SCHEDULE_ENGINE_H
#define HYDRA_PUMP_SCHEDULE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"

typedef enum {
    PUMP_SCHEDULE_PHASE_NONE = 0,
    PUMP_SCHEDULE_PHASE_START = 1,
    PUMP_SCHEDULE_PHASE_END = 2,
} pump_schedule_phase_t;

typedef struct {
    bool    running;
    uint8_t schedule_count;
    char    next_action[128];
} pump_schedule_engine_status_t;

int pump_schedule_resolve_trigger_minute(const config_pump_schedule_t *s,
                                         bool start_trigger,
                                         int sunrise_minute,
                                         int sunset_minute);
pump_schedule_phase_t pump_schedule_due_phase(const config_pump_schedule_t *s,
                                              int previous_minute,
                                              int now_minute,
                                              int start_minute,
                                              int end_minute);

#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t pump_schedule_engine_init(void);
esp_err_t pump_schedule_engine_reconfigure(void);
void pump_schedule_engine_get_status(pump_schedule_engine_status_t *out);

#endif

#endif /* HYDRA_PUMP_SCHEDULE_ENGINE_H */
