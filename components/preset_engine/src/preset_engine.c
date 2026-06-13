#include "preset_engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Preset values come from docs/esp32-hydra64hd-controller-plan.md
 * "Preset System" section (L661-L733). Order matches channel_model_all():
 *   brightness, coolwhite, blue, deepred, violet, uv, green, royalblue, moonlight
 */
static const channel_state_t k_preset_off = {
    .values = { 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

static const channel_state_t k_preset_on = {
    .values = { 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 }
};

/* spec L693-L703: brightness=1000, moonlight=50, all others=0 */
static const channel_state_t k_preset_moonlight = {
    .values = { 1000, 0, 0, 0, 0, 0, 0, 0, 50 }
};

/* spec L707-L716: brightness=1000, blue=40, violet=20, royalblue=80, others=0 */
static const channel_state_t k_preset_blue_moonlight = {
    /*           bright  cw  blue  dr   viol  uv  grn  rblue  moon */
    .values = { 1000,    0,  40,   0,   20,   0,  0,   80,    0 }
};

/* spec L721-L732: brightness=1000, every color channel at 250 */
static const channel_state_t k_preset_test_25 = {
    .values = { 1000, 250, 250, 250, 250, 250, 250, 250, 250 }
};

static const preset_info_t k_presets[PRESET_COUNT] = {
    { PRESET_OFF,            "off",            "Off"              },
    { PRESET_ON,             "on",             "On"               },
    { PRESET_MOONLIGHT,      "moonlight",      "White Moonlight"  },
    { PRESET_BLUE_MOONLIGHT, "blue_moonlight", "Blue Moonlight"   },
    { PRESET_TEST_25,        "test_25",        "Test 25%"         },
};

static const channel_state_t *state_for(preset_id_t id)
{
    switch (id) {
        case PRESET_OFF:            return &k_preset_off;
        case PRESET_ON:             return &k_preset_on;
        case PRESET_MOONLIGHT:      return &k_preset_moonlight;
        case PRESET_BLUE_MOONLIGHT: return &k_preset_blue_moonlight;
        case PRESET_TEST_25:        return &k_preset_test_25;
        default:                    return NULL;
    }
}

int preset_expand(preset_id_t id, channel_state_t *out)
{
    if (!out) return -1;
    const channel_state_t *src = state_for(id);
    if (!src) return -1;
    *out = *src;
    return 0;
}

const char *preset_name(preset_id_t id)
{
    for (size_t i = 0; i < PRESET_COUNT; ++i) {
        if (k_presets[i].id == id) return k_presets[i].name;
    }
    return NULL;
}

preset_id_t preset_by_name(const char *name)
{
    if (!name) return PRESET_NONE;
    for (size_t i = 0; i < PRESET_COUNT; ++i) {
        if (strcmp(k_presets[i].name, name) == 0) return k_presets[i].id;
    }
    return PRESET_NONE;
}

const preset_info_t *preset_all(size_t *count)
{
    if (count) *count = PRESET_COUNT;
    return k_presets;
}
