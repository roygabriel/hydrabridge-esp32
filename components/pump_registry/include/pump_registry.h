#ifndef HYDRA_PUMP_REGISTRY_H
#define HYDRA_PUMP_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "light_registry.h"

#define PUMP_REGISTRY_CAPACITY      4
#define PUMP_ID_LEN                 33
#define PUMP_NAME_LEN               33
#define PUMP_SERIAL_LEN             33

typedef enum {
    PUMP_PROTOCOL_UNKNOWN = 0,
    PUMP_PROTOCOL_EXPERIMENTAL_MOBIUS = 1,
} pump_protocol_status_t;

typedef enum {
    PUMP_MODE_UNKNOWN = 0,
    PUMP_MODE_CONSTANT = 1,
    PUMP_MODE_LAGOON = 2,
    PUMP_MODE_REEF_CREST = 3,
    PUMP_MODE_NUTRIENT_TRANSPORT = 4,
    PUMP_MODE_TIDAL_SWELL = 5,
    PUMP_MODE_SHORT_PULSE = 6,
    PUMP_MODE_GYRE = 7,
    PUMP_MODE_TRANSITION = 8,
    PUMP_MODE_EXPANDING_PULSE = 9,
    PUMP_MODE_SYNC = 10,
    PUMP_MODE_ECOSMART_BACK = 12,
    PUMP_MODE_FEED = 13,
    PUMP_MODE_BATTERY_BACKUP = 14,
    PUMP_MODE_RANDOM = 15,
    PUMP_MODE_PULSE = 16,
} pump_mode_t;

typedef struct {
    char                   pump_id[PUMP_ID_LEN];
    char                   display_name[PUMP_NAME_LEN];
    char                   serial[PUMP_SERIAL_LEN];
    uint8_t                ble_addr[BLE_ADDR_BYTES];
    ble_addr_type_t        ble_addr_type;
    uint16_t               model;
    bool                   enabled;
    int8_t                 last_seen_rssi;
    pump_mode_t            last_mode;
    uint8_t                last_speed_percent;
    pump_protocol_status_t protocol_status;
} registered_pump_t;

void pump_registry_reset(void);

size_t pump_registry_count(void);
int pump_registry_add(const registered_pump_t *pump);
int pump_registry_remove(const char *pump_id);
const registered_pump_t *pump_registry_get(const char *pump_id);
const registered_pump_t *pump_registry_get_by_serial(const char *serial);
const registered_pump_t *pump_registry_get_by_addr(const uint8_t *addr);
const registered_pump_t *pump_registry_at(size_t idx);
int pump_registry_set_status(const char *pump_id, pump_mode_t mode, uint8_t speed_percent);
int pump_registry_set_rssi(const char *pump_id, int8_t rssi);
int pump_registry_set_enabled(const char *pump_id, bool enabled);
int pump_registry_rename(const char *pump_id, const char *display_name);
int pump_registry_update_discovery(const char *pump_id,
                                   const uint8_t addr[BLE_ADDR_BYTES],
                                   ble_addr_type_t addr_type,
                                   int8_t rssi);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t pump_registry_init(void);
esp_err_t pump_registry_save(void);
#endif

#endif /* HYDRA_PUMP_REGISTRY_H */
