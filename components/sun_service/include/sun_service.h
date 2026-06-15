#ifndef HYDRA_SUN_SERVICE_H
#define HYDRA_SUN_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"

typedef struct {
    bool    enabled;
    bool    valid;
    char    location_label[CONFIG_LOCATION_LABEL_LEN];
    int32_t latitude_e7;
    int32_t longitude_e7;
    int     year;
    int     month;
    int     day;
    int     sunrise_minute; /* local minute of day */
    int     sunset_minute;  /* local minute of day */
    char    sunrise_local[16];
    char    sunset_local[16];
} sun_service_status_t;

bool sun_calc_utc_minutes(int year, int month, int day,
                          int32_t latitude_e7, int32_t longitude_e7,
                          int *sunrise_utc_minute,
                          int *sunset_utc_minute);

#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t sun_service_init(void);
esp_err_t sun_service_reconfigure(void);
void sun_service_get_status(sun_service_status_t *out);
bool sun_service_get_today(int *sunrise_minute, int *sunset_minute);
#endif

#endif /* HYDRA_SUN_SERVICE_H */
