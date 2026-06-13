#include "channel_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "channel_model";
#endif

/* Canonical Hydra 64HD channel set in fixture order. Visual IDs and
 * order come from docs/myai-ble-reverse-engineering.md (the captured
 * SupportedColorChannels response 01 10 11 19 17 15 13 12 1e) and from
 * docs/esp32-hydra64hd-controller-plan.md "Channel Abstraction"
 * section. Do NOT reorder -- the LiveDemoScene payload builder emits
 * triples in this exact order. */
static const channel_def_t k_channels[CHANNEL_COUNT] = {
    { "brightness", "Brightness", 0x01, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_MASTER },
    { "coolwhite",  "Cool White", 0x10, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "blue",       "Blue",       0x11, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "deepred",    "Deep Red",   0x19, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "violet",     "Violet",     0x17, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "uv",         "UV",         0x15, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "green",      "Green",      0x13, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "royalblue",  "Royal Blue", 0x12, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_COLOR  },
    { "moonlight",  "Moonlight",  0x1e, 0, CHANNEL_VALUE_MAX, CHANNEL_ROLE_MOON   },
};

const channel_def_t *channel_model_all(void)
{
    return k_channels;
}

static int find_index_by_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (strcmp(k_channels[i].name, name) == 0) return i;
    }
    return -1;
}

const channel_def_t *channel_model_by_name(const char *name)
{
    int idx = find_index_by_name(name);
    return idx < 0 ? NULL : &k_channels[idx];
}

const channel_def_t *channel_model_by_visual_id(uint8_t visual_id)
{
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (k_channels[i].visual_id == visual_id) return &k_channels[i];
    }
    return NULL;
}

void channel_state_zero(channel_state_t *out)
{
    if (!out) return;
    for (int i = 0; i < CHANNEL_COUNT; ++i) out->values[i] = 0;
}

bool channel_value_is_valid(uint16_t v)
{
    return v <= CHANNEL_VALUE_MAX;
}

int channel_state_build(channel_state_t *out,
                        const channel_state_t *base,
                        bool replace,
                        const channel_kv_t *kvs,
                        size_t kv_count)
{
    if (!out) return -1;
    if (!replace && !base) return -1;
    if (kv_count > 0 && !kvs) return -1;

    /* First pass: validate every update. If anything is wrong, bail
     * out before mutating `out`. */
    for (size_t i = 0; i < kv_count; ++i) {
        int idx = find_index_by_name(kvs[i].name);
        if (idx < 0) return -1;
        if (!channel_value_is_valid(kvs[i].value)) return -1;
    }

    channel_state_t scratch;
    if (replace) {
        channel_state_zero(&scratch);
    } else {
        scratch = *base;
    }

    for (size_t i = 0; i < kv_count; ++i) {
        int idx = find_index_by_name(kvs[i].name); /* known valid */
        scratch.values[idx] = kvs[i].value;
    }

    *out = scratch;
    return 0;
}

#ifdef ESP_PLATFORM
esp_err_t channel_model_init(void)
{
    ESP_LOGI(TAG, "init %d canonical channels", CHANNEL_COUNT);
    return ESP_OK;
}
#endif
