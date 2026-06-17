#ifndef HYDRA_CONFIG_STORE_H
#define HYDRA_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* config_store
 * ============
 * Persistent configuration for controller / modbus / wifi / mqtt / time /
 * sun / schedules + user color profiles (named 9-channel intensity mixes
 * for LiveDemoScene).
 * Each record lives in its own NVS namespace as a single blob:
 *   [uint32_t schema_version][packed struct bytes]
 *
 * Loads fall back to defaults_X() on any of:
 *   - namespace missing
 *   - blob key missing
 *   - schema version mismatch
 *   - blob size mismatch (e.g. struct grew)
 *
 * Defaults are exposed as pure-C functions so they can be tested on the
 * host without an NVS implementation. Light + group records are NOT
 * here -- they live in light_registry.
 */

#define CONFIG_SCHEMA_CONTROLLER  1
#define CONFIG_SCHEMA_MODBUS      1
#define CONFIG_SCHEMA_WIFI        2
#define CONFIG_SCHEMA_MQTT        2
#define CONFIG_SCHEMA_PROFILES    2
#define CONFIG_SCHEMA_TIME        1
#define CONFIG_SCHEMA_SUN         1
#define CONFIG_SCHEMA_SCHEDULES   1
#define CONFIG_SCHEMA_PUMP_SCHEDULES 2

#define MAX_USER_PROFILES         8
#define MAX_SCHEDULES             12
#define MAX_PUMP_SCHEDULES        12
#define CONFIG_PROFILE_NAME_LEN   17  /* includes NUL */
#define CONFIG_PROFILE_DESC_LEN   129 /* includes NUL */

/* ---- string-size budgets (include the trailing NUL) ---- */
#define CONFIG_CONTROLLER_ID_LEN   33
#define CONFIG_DEVICE_NAME_LEN     33
#define CONFIG_TIMEZONE_LEN        17
#define CONFIG_FIRMWARE_VER_LEN    17
#define CONFIG_WIFI_SSID_LEN       33  /* 802.11 max 32 + NUL */
#define CONFIG_WIFI_PSK_LEN        65  /* WPA2 max 63 + NUL  */
#define CONFIG_WIFI_AP_SSID_LEN    33
#define CONFIG_WIFI_AP_PSK_LEN     65
#define CONFIG_MQTT_HOST_LEN       65
#define CONFIG_MQTT_USER_LEN       33
#define CONFIG_MQTT_PSK_LEN        65
#define CONFIG_MQTT_CLIENT_ID_LEN  33
#define CONFIG_MQTT_TOPIC_LEN      33
#define CONFIG_MQTT_HA_PREFIX_LEN  33
#define CONFIG_TIME_SERVER_LEN     65
#define CONFIG_TZ_LEN              65
#define CONFIG_LOCATION_LABEL_LEN  33
#define CONFIG_SCHEDULE_ID_LEN     17
#define CONFIG_SCHEDULE_NAME_LEN   33
#define CONFIG_SCHEDULE_TARGET_LEN 33

typedef enum {
    MODBUS_PARITY_NONE = 0,
    MODBUS_PARITY_EVEN = 1,
    MODBUS_PARITY_ODD  = 2,
} modbus_parity_t;

typedef struct {
    char     controller_id[CONFIG_CONTROLLER_ID_LEN];
    char     device_name[CONFIG_DEVICE_NAME_LEN];
    char     timezone[CONFIG_TIMEZONE_LEN];
    char     firmware_version[CONFIG_FIRMWARE_VER_LEN];
    bool     ota_enabled;
    uint8_t  max_registered_lights;
    uint32_t ble_idle_disconnect_ms;
    uint8_t  ble_command_concurrency;
} config_controller_t;

typedef struct {
    bool     enabled;
    bool     master_mode_enabled;
    uint8_t  slave_address;
    uint32_t baud_rate;
    uint8_t  data_bits;
    modbus_parity_t parity;
    uint8_t  stop_bits;
    uint8_t  uart_port;
    int8_t   tx_pin;
    int8_t   rx_pin;
    int8_t   rts_de_pin;
    uint32_t response_timeout_ms;
    uint32_t command_watchdog_ms;
} config_modbus_t;

typedef struct {
    bool     enabled;
    char     ssid[CONFIG_WIFI_SSID_LEN];
    char     password[CONFIG_WIFI_PSK_LEN];
    bool     ap_fallback_enabled;
    char     ap_ssid[CONFIG_WIFI_AP_SSID_LEN];
    char     ap_password[CONFIG_WIFI_AP_PSK_LEN];
} config_wifi_t;

typedef struct {
    bool     enabled;
    char     host[CONFIG_MQTT_HOST_LEN];
    uint16_t port;
    bool     use_tls;
    char     username[CONFIG_MQTT_USER_LEN];
    char     password[CONFIG_MQTT_PSK_LEN];
    char     client_id[CONFIG_MQTT_CLIENT_ID_LEN];
    uint16_t keepalive_sec;
    char     base_topic[CONFIG_MQTT_TOPIC_LEN];
    bool     home_assistant_discovery;
    char     home_assistant_prefix[CONFIG_MQTT_HA_PREFIX_LEN];
} config_mqtt_t;

typedef struct {
    bool     enabled;
    char     server[CONFIG_TIME_SERVER_LEN];
    char     timezone[CONFIG_TZ_LEN];  /* POSIX TZ string for setenv("TZ", ...). */
} config_time_t;

typedef struct {
    bool     enabled;
    char     location_label[CONFIG_LOCATION_LABEL_LEN];
    int32_t  latitude_e7;   /* degrees * 10,000,000 */
    int32_t  longitude_e7;  /* degrees * 10,000,000 */
} config_sun_t;

typedef struct {
    char     name[CONFIG_PROFILE_NAME_LEN];
    char     description[CONFIG_PROFILE_DESC_LEN];
    uint16_t intensities[9];  /* 0..1000 per channel, in canonical SupportedColorChannels order (see channel_model) */
} user_profile_t;

typedef struct {
    uint8_t        count;
    user_profile_t profiles[MAX_USER_PROFILES];
} config_profiles_t;

typedef enum {
    CONFIG_SCHEDULE_TARGET_LIGHT = 0,
    CONFIG_SCHEDULE_TARGET_GROUP = 1,
} config_schedule_target_t;

typedef enum {
    CONFIG_SCHEDULE_TRIGGER_FIXED   = 0,
    CONFIG_SCHEDULE_TRIGGER_SUNRISE = 1,
    CONFIG_SCHEDULE_TRIGGER_SUNSET  = 2,
} config_schedule_trigger_t;

typedef struct {
    bool      enabled;
    char      schedule_id[CONFIG_SCHEDULE_ID_LEN];
    char      name[CONFIG_SCHEDULE_NAME_LEN];
    uint8_t   target_type;  /* config_schedule_target_t */
    char      target_id[CONFIG_SCHEDULE_TARGET_LEN];
    char      profile_name[CONFIG_PROFILE_NAME_LEN];
    uint8_t   intensity_percent;      /* 0..100 */
    uint8_t   end_intensity_percent;  /* 0..100 */
    uint8_t   start_trigger;          /* config_schedule_trigger_t */
    uint8_t   end_trigger;            /* config_schedule_trigger_t */
    uint16_t  start_minute;           /* 0..1439, only for fixed trigger */
    uint16_t  end_minute;             /* 0..1439, only for fixed trigger */
    int16_t   start_offset_min;       /* sunrise/sunset offset */
    int16_t   end_offset_min;         /* sunrise/sunset offset */
    uint16_t  ramp_up_min;
    uint16_t  ramp_down_min;
} config_schedule_t;

typedef struct {
    uint8_t           count;
    config_schedule_t schedules[MAX_SCHEDULES];
} config_schedules_t;

typedef enum {
    CONFIG_PUMP_MODE_CONSTANT = 1,
    CONFIG_PUMP_MODE_LAGOON = 2,
    CONFIG_PUMP_MODE_REEF_CREST = 3,
    CONFIG_PUMP_MODE_NUTRIENT_TRANSPORT = 4,
    CONFIG_PUMP_MODE_TIDAL_SWELL = 5,
    CONFIG_PUMP_MODE_SHORT_PULSE = 6,
    CONFIG_PUMP_MODE_GYRE = 7,
    CONFIG_PUMP_MODE_TRANSITION = 8,
    CONFIG_PUMP_MODE_EXPANDING_PULSE = 9,
    CONFIG_PUMP_MODE_SYNC = 10,
    CONFIG_PUMP_MODE_ECOSMART_BACK = 12,
    CONFIG_PUMP_MODE_FEED = 13,
    CONFIG_PUMP_MODE_BATTERY_BACKUP = 14,
    CONFIG_PUMP_MODE_RANDOM = 15,
    CONFIG_PUMP_MODE_PULSE = 16,
} config_pump_mode_t;

typedef struct {
    bool      enabled;
    char      schedule_id[CONFIG_SCHEDULE_ID_LEN];
    char      name[CONFIG_SCHEDULE_NAME_LEN];
    char      target_id[CONFIG_SCHEDULE_TARGET_LEN];
    uint8_t   active_mode;           /* config_pump_mode_t */
    uint8_t   active_speed_percent;  /* 0..100 */
    uint8_t   active_min_speed_percent;
    uint8_t   active_variance_percent;
    uint16_t  active_on_time_ms;
    uint16_t  active_off_time_ms;
    uint8_t   end_mode;              /* config_pump_mode_t */
    uint8_t   end_speed_percent;     /* 0..100 */
    uint8_t   end_min_speed_percent;
    uint8_t   end_variance_percent;
    uint16_t  end_on_time_ms;
    uint16_t  end_off_time_ms;
    uint8_t   start_trigger;         /* config_schedule_trigger_t */
    uint8_t   end_trigger;           /* config_schedule_trigger_t */
    uint16_t  start_minute;          /* 0..1439, only for fixed trigger */
    uint16_t  end_minute;            /* 0..1439, only for fixed trigger */
    int16_t   start_offset_min;      /* sunrise/sunset offset */
    int16_t   end_offset_min;        /* sunrise/sunset offset */
} config_pump_schedule_t;

typedef struct {
    uint8_t                count;
    config_pump_schedule_t schedules[MAX_PUMP_SCHEDULES];
} config_pump_schedules_t;

/* ---- defaults factories (pure C, host-testable) ---- */
void config_defaults_controller(config_controller_t *out);
void config_defaults_modbus(config_modbus_t *out);
void config_defaults_wifi(config_wifi_t *out);
void config_defaults_mqtt(config_mqtt_t *out);
void config_defaults_profiles(config_profiles_t *out);
void config_defaults_time(config_time_t *out);
void config_defaults_sun(config_sun_t *out);
void config_defaults_schedules(config_schedules_t *out);
void config_defaults_pump_schedules(config_pump_schedules_t *out);

/* ---- ESP-IDF NVS API (only available in the firmware build) ---- */
#ifdef ESP_PLATFORM
#include "esp_err.h"

esp_err_t config_store_init(void);

esp_err_t config_store_load_controller(config_controller_t *out);
esp_err_t config_store_save_controller(const config_controller_t *in);

esp_err_t config_store_load_modbus(config_modbus_t *out);
esp_err_t config_store_save_modbus(const config_modbus_t *in);

esp_err_t config_store_load_wifi(config_wifi_t *out);
esp_err_t config_store_save_wifi(const config_wifi_t *in);

esp_err_t config_store_load_mqtt(config_mqtt_t *out);
esp_err_t config_store_save_mqtt(const config_mqtt_t *in);

esp_err_t config_store_load_profiles(config_profiles_t *out);
esp_err_t config_store_save_profiles(const config_profiles_t *in);

esp_err_t config_store_load_time(config_time_t *out);
esp_err_t config_store_save_time(const config_time_t *in);

esp_err_t config_store_load_sun(config_sun_t *out);
esp_err_t config_store_save_sun(const config_sun_t *in);

esp_err_t config_store_load_schedules(config_schedules_t *out);
esp_err_t config_store_save_schedules(const config_schedules_t *in);

esp_err_t config_store_load_pump_schedules(config_pump_schedules_t *out);
esp_err_t config_store_save_pump_schedules(const config_pump_schedules_t *in);

/* For tests / factory reset: erase every config namespace. */
esp_err_t config_store_factory_reset(void);
#endif /* ESP_PLATFORM */

#endif /* HYDRA_CONFIG_STORE_H */
