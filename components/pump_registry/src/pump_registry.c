#include "pump_registry.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
static const char *TAG = "pump_registry";

#define NS_PUMPS "pumps"
#define KEY_BLOB "tbl"
#define PUMPS_BLOB_VER 1
#endif

static registered_pump_t g_pumps[PUMP_REGISTRY_CAPACITY];
static size_t            g_pump_count;

void pump_registry_reset(void)
{
    memset(g_pumps, 0, sizeof g_pumps);
    g_pump_count = 0;
}

size_t pump_registry_count(void) { return g_pump_count; }

static int find_pump_idx(const char *pump_id)
{
    if (!pump_id) return -1;
    for (size_t i = 0; i < g_pump_count; ++i) {
        if (strncmp(g_pumps[i].pump_id, pump_id, PUMP_ID_LEN) == 0) return (int)i;
    }
    return -1;
}

int pump_registry_add(const registered_pump_t *pump)
{
    if (!pump || !pump->pump_id[0]) return -1;
    if (g_pump_count >= PUMP_REGISTRY_CAPACITY) return -1;
    if (find_pump_idx(pump->pump_id) >= 0) return -1;
    g_pumps[g_pump_count] = *pump;
    g_pumps[g_pump_count].pump_id[PUMP_ID_LEN - 1] = '\0';
    g_pumps[g_pump_count].display_name[PUMP_NAME_LEN - 1] = '\0';
    g_pumps[g_pump_count].serial[PUMP_SERIAL_LEN - 1] = '\0';
    ++g_pump_count;
    return 0;
}

int pump_registry_remove(const char *pump_id)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0) return -1;
    for (size_t i = (size_t)idx; i + 1 < g_pump_count; ++i) {
        g_pumps[i] = g_pumps[i + 1];
    }
    --g_pump_count;
    memset(&g_pumps[g_pump_count], 0, sizeof g_pumps[g_pump_count]);
    return 0;
}

const registered_pump_t *pump_registry_get(const char *pump_id)
{
    int idx = find_pump_idx(pump_id);
    return idx < 0 ? NULL : &g_pumps[idx];
}

const registered_pump_t *pump_registry_get_by_serial(const char *serial)
{
    if (!serial) return NULL;
    for (size_t i = 0; i < g_pump_count; ++i) {
        if (strncmp(g_pumps[i].serial, serial, PUMP_SERIAL_LEN) == 0) return &g_pumps[i];
    }
    return NULL;
}

const registered_pump_t *pump_registry_get_by_addr(const uint8_t *addr)
{
    if (!addr) return NULL;
    for (size_t i = 0; i < g_pump_count; ++i) {
        if (memcmp(g_pumps[i].ble_addr, addr, BLE_ADDR_BYTES) == 0) return &g_pumps[i];
    }
    return NULL;
}

const registered_pump_t *pump_registry_at(size_t idx)
{
    if (idx >= g_pump_count) return NULL;
    return &g_pumps[idx];
}

int pump_registry_set_status(const char *pump_id, pump_mode_t mode, uint8_t speed_percent)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0 || speed_percent > 100) return -1;
    g_pumps[idx].last_mode = mode;
    g_pumps[idx].last_speed_percent = speed_percent;
    return 0;
}

int pump_registry_set_rssi(const char *pump_id, int8_t rssi)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0) return -1;
    g_pumps[idx].last_seen_rssi = rssi;
    return 0;
}

int pump_registry_set_enabled(const char *pump_id, bool enabled)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0) return -1;
    g_pumps[idx].enabled = enabled;
    return 0;
}

int pump_registry_rename(const char *pump_id, const char *display_name)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0 || !display_name) return -1;

    while (*display_name && isspace((unsigned char)*display_name)) ++display_name;
    size_t len = strnlen(display_name, PUMP_NAME_LEN);
    while (len > 0 && isspace((unsigned char)display_name[len - 1])) --len;
    if (len == 0) return -1;
    if (len >= PUMP_NAME_LEN) len = PUMP_NAME_LEN - 1;

    memcpy(g_pumps[idx].display_name, display_name, len);
    g_pumps[idx].display_name[len] = '\0';
    return 0;
}

int pump_registry_update_discovery(const char *pump_id,
                                   const uint8_t addr[BLE_ADDR_BYTES],
                                   ble_addr_type_t addr_type,
                                   int8_t rssi)
{
    int idx = find_pump_idx(pump_id);
    if (idx < 0 || !addr) return -1;
    memcpy(g_pumps[idx].ble_addr, addr, BLE_ADDR_BYTES);
    g_pumps[idx].ble_addr_type = addr_type;
    g_pumps[idx].last_seen_rssi = rssi;
    return 0;
}

#ifdef ESP_PLATFORM

typedef struct __attribute__((packed)) {
    uint32_t schema_version;
    uint32_t count;
} blob_header_t;

static esp_err_t save_blob(const char *ns, uint32_t version,
                           const void *items, size_t item_size, size_t count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t total = sizeof(blob_header_t) + count * item_size;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }

    blob_header_t hdr = { version, (uint32_t)count };
    memcpy(buf, &hdr, sizeof hdr);
    if (count) memcpy(buf + sizeof hdr, items, count * item_size);

    err = nvs_set_blob(h, KEY_BLOB, buf, total);
    free(buf);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t load_blob(const char *ns, uint32_t expected_version,
                           void *items, size_t item_size, size_t capacity,
                           size_t *count_out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t got = 0;
    err = nvs_get_blob(h, KEY_BLOB, NULL, &got);
    if (err != ESP_OK) { nvs_close(h); return err; }
    if (got < sizeof(blob_header_t)) { nvs_close(h); return ESP_ERR_INVALID_SIZE; }

    uint8_t *buf = (uint8_t *)malloc(got);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    err = nvs_get_blob(h, KEY_BLOB, buf, &got);
    nvs_close(h);
    if (err != ESP_OK) { free(buf); return err; }

    blob_header_t hdr;
    memcpy(&hdr, buf, sizeof hdr);
    if (hdr.schema_version != expected_version) { free(buf); return ESP_ERR_INVALID_VERSION; }
    if (hdr.count > capacity) { free(buf); return ESP_ERR_INVALID_SIZE; }
    if (got != sizeof(blob_header_t) + hdr.count * item_size) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    if (hdr.count) memcpy(items, buf + sizeof hdr, hdr.count * item_size);
    *count_out = hdr.count;
    free(buf);
    return ESP_OK;
}

esp_err_t pump_registry_init(void)
{
    pump_registry_reset();
    size_t n = 0;
    esp_err_t err = load_blob(NS_PUMPS, PUMPS_BLOB_VER,
                              g_pumps, sizeof g_pumps[0],
                              PUMP_REGISTRY_CAPACITY, &n);
    if (err == ESP_OK) {
        g_pump_count = n;
    } else {
        ESP_LOGW(TAG, "pumps load err=0x%x; starting empty", err);
    }
    ESP_LOGI(TAG, "init: %u pumps", (unsigned)g_pump_count);
    return ESP_OK;
}

esp_err_t pump_registry_save(void)
{
    return save_blob(NS_PUMPS, PUMPS_BLOB_VER,
                     g_pumps, sizeof g_pumps[0], g_pump_count);
}

#endif /* ESP_PLATFORM */
