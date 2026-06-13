#ifndef HYDRA_CHANNEL_MODEL_H
#define HYDRA_CHANNEL_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* channel_model
 * =============
 * Canonical channel set for the Hydra 64HD. Defines the names that the
 * web UI / MQTT / Modbus surfaces use ("brightness", "coolwhite", ...),
 * maps each to its protocol visual_id byte (0x01, 0x10, ...), and
 * provides helpers to build a complete 9-element value vector from
 * partial (name, value) updates.
 *
 * Only this module knows about visual_ids and channel ordering. Every
 * other module passes channel_state_t around.
 */

#define CHANNEL_COUNT       9
#define CHANNEL_VALUE_MAX   1000

typedef enum {
    CHANNEL_ROLE_MASTER = 0,  /* Brightness */
    CHANNEL_ROLE_COLOR  = 1,
    CHANNEL_ROLE_MOON   = 2,  /* Moonlight */
} channel_role_t;

typedef struct {
    const char    *name;       /* canonical lowercase, e.g. "brightness" */
    const char    *label;      /* user-facing, e.g. "Brightness"         */
    uint8_t        visual_id;  /* protocol byte, e.g. 0x01               */
    uint16_t       min;        /* 0 in v1                                */
    uint16_t       max;        /* 1000 in v1                             */
    channel_role_t role;
} channel_def_t;

/* The 9 channels in fixture order. Index 0 = brightness, ..., index 8 =
 * moonlight. The hydra64hd_protocol payload builder iterates this list
 * to emit the LiveDemoScene triples in the order the fixture expects. */
const channel_def_t *channel_model_all(void);

/* Look up by canonical name. Returns NULL on unknown. */
const channel_def_t *channel_model_by_name(const char *name);

/* Look up by visual_id. Returns NULL on unknown. */
const channel_def_t *channel_model_by_visual_id(uint8_t visual_id);

/* Full channel state, indexed by the same order as channel_model_all(). */
typedef struct {
    uint16_t values[CHANNEL_COUNT];
} channel_state_t;

void channel_state_zero(channel_state_t *out);
bool channel_value_is_valid(uint16_t v);

/* A single (name, value) update. */
typedef struct {
    const char *name;
    uint16_t    value;
} channel_kv_t;

/* Build a complete state from a list of partial updates.
 *   replace=true  : missing channels default to 0
 *   replace=false : missing channels default to *base (must be non-NULL)
 * Returns 0 on success. On any unknown name or out-of-range value,
 * returns -1 and *out is not modified. */
int channel_state_build(channel_state_t *out,
                        const channel_state_t *base,
                        bool replace,
                        const channel_kv_t *kvs,
                        size_t kv_count);

#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t channel_model_init(void);
#endif

#endif /* HYDRA_CHANNEL_MODEL_H */
