#ifndef HYDRA_TIME_SERVICE_H
#define HYDRA_TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "config_store.h"

typedef struct {
    bool    enabled;
    bool    synced;
    char    server[CONFIG_TIME_SERVER_LEN];
    char    timezone[CONFIG_TZ_LEN];
    int64_t current_epoch;
    int64_t last_sync_epoch;
    char    current_local[32];
    char    last_sync_local[32];
} time_service_status_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t time_service_init(void);
esp_err_t time_service_reconfigure(void);
void time_service_get_status(time_service_status_t *out);
bool time_service_is_synced(void);
time_t time_service_now(void);
#endif

#endif /* HYDRA_TIME_SERVICE_H */
