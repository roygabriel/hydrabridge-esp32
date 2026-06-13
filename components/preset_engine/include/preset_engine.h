#ifndef HYDRA_PRESET_ENGINE_H
#define HYDRA_PRESET_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "channel_model.h"

/* preset_engine
 * =============
 * Built-in named channel-state templates. Presets are expanded BEFORE a
 * command reaches BLE so the codec layer always sees a complete
 * 9-channel state. User-editable presets in NVS are out of scope for
 * v1.
 *
 * Preset IDs match the Modbus enum encoding documented in spec L875-880.
 */

typedef enum {
    PRESET_NONE           = 0,
    PRESET_OFF            = 1,
    PRESET_ON             = 2,
    PRESET_MOONLIGHT      = 3,
    PRESET_BLUE_MOONLIGHT = 4,
    PRESET_TEST_25        = 5,
} preset_id_t;

#define PRESET_COUNT 5  /* not counting PRESET_NONE */

typedef struct {
    preset_id_t  id;
    const char  *name;         /* canonical lowercase, e.g. "moonlight" */
    const char  *display_name; /* e.g. "Moonlight" */
} preset_info_t;

int preset_expand(preset_id_t id, channel_state_t *out);
const char *preset_name(preset_id_t id);
preset_id_t preset_by_name(const char *name);
const preset_info_t *preset_all(size_t *count);

#endif /* HYDRA_PRESET_ENGINE_H */
