#ifndef HYDRA_SCHEDULE_ENGINE_H
#define HYDRA_SCHEDULE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"

typedef struct {
    bool    running;
    uint8_t schedule_count;
    char    next_action[128];
} schedule_engine_status_t;

int schedule_resolve_trigger_minute(const config_schedule_t *s,
                                    bool start_trigger,
                                    int sunrise_minute,
                                    int sunset_minute);
int schedule_active_intensity_percent(const config_schedule_t *s,
                                      int now_minute,
                                      int start_minute,
                                      int end_minute);

#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t schedule_engine_init(void);
esp_err_t schedule_engine_reconfigure(void);
void schedule_engine_get_status(schedule_engine_status_t *out);

#endif

#endif /* HYDRA_SCHEDULE_ENGINE_H */
