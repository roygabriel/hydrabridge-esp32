#include "schedule_engine.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "channel_model.h"

static int clamp_percent(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static int wrap_minute(int minute)
{
    while (minute < 0) minute += 1440;
    while (minute >= 1440) minute -= 1440;
    return minute;
}

static int minutes_between(int from, int to)
{
    int d = to - from;
    if (d < 0) d += 1440;
    return d;
}

int schedule_resolve_trigger_minute(const config_schedule_t *s,
                                    bool start_trigger,
                                    int sunrise_minute,
                                    int sunset_minute)
{
    if (!s) return -1;
    uint8_t trig = start_trigger ? s->start_trigger : s->end_trigger;
    uint16_t fixed = start_trigger ? s->start_minute : s->end_minute;
    int16_t offset = start_trigger ? s->start_offset_min : s->end_offset_min;
    switch (trig) {
        case CONFIG_SCHEDULE_TRIGGER_FIXED:
            return fixed < 1440 ? (int)fixed : -1;
        case CONFIG_SCHEDULE_TRIGGER_SUNRISE:
            return sunrise_minute >= 0 ? wrap_minute(sunrise_minute + offset) : -1;
        case CONFIG_SCHEDULE_TRIGGER_SUNSET:
            return sunset_minute >= 0 ? wrap_minute(sunset_minute + offset) : -1;
        default:
            return -1;
    }
}

int schedule_active_intensity_percent(const config_schedule_t *s,
                                      int now_minute,
                                      int start_minute,
                                      int end_minute)
{
    if (!s || now_minute < 0 || now_minute >= 1440 ||
        start_minute < 0 || end_minute < 0) {
        return -1;
    }

    int duration = minutes_between(start_minute, end_minute);
    if (duration == 0) duration = 1440;
    int elapsed = minutes_between(start_minute, now_minute);
    if (elapsed > duration) return -1;
    if (elapsed == duration) return clamp_percent(s->end_intensity_percent);

    int target = clamp_percent(s->intensity_percent);
    int base = clamp_percent(s->end_intensity_percent);
    int ramp_up = s->ramp_up_min;
    int ramp_down = s->ramp_down_min;
    if (ramp_up > duration) ramp_up = duration;
    if (ramp_down > duration) ramp_down = duration;

    if (ramp_up > 0 && elapsed < ramp_up) {
        return base + ((target - base) * elapsed) / ramp_up;
    }

    int remaining = duration - elapsed;
    if (ramp_down > 0 && remaining < ramp_down) {
        return base + ((target - base) * remaining) / ramp_down;
    }

    return target;
}

#ifdef ESP_PLATFORM

#include "command_engine.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sun_service.h"

static const char *TAG = "schedule_engine";

typedef struct {
    char schedule_id[CONFIG_SCHEDULE_ID_LEN];
    int  last_intensity;
    int  last_yday;
} schedule_runtime_t;

static config_schedules_t s_cfg;
static schedule_runtime_t s_runtime[MAX_SCHEDULES];
static TaskHandle_t s_task;
static bool s_running;
static char s_next_action[128] = "No schedules";

static const user_profile_t k_builtin_profiles[] = {
    { "Zoa Pop",        "Best for zoas, mushrooms, LPS viewing, and fluorescence.", { 1000, 150, 1000,  50, 1000,  950,  50, 1000,   0 } },
    { "AB Plus",        "Good balanced profile for mixed reef growth and color.",    { 1000, 250, 1000, 100,  900,  900, 100, 1000,   0 } },
    { "Mixed Reef",     "More natural look while still reef-safe.",                  { 1000, 400,  900, 100,  800,  750, 150,  900,   0 } },
    { "Frag Growth",    "Higher blue and violet, moderate white for frag racks.",    { 1000, 200, 1000,  50,  900,  850,  50, 1000,   0 } },
    { "Evening",        "After-work viewing without blasting PAR.",                  { 1000,  50,  650,   0,  550,  450,   0,  700,   0 } },
    { "Photo Mode",     "Better for coral photos with less overwhelming blue.",      { 1000, 450,  750,  80,  500,  400, 100,  750,   0 } },
    { "Moonlight Only", "Very low moonlight for a short night window.",              { 1000,   0,    0,   0,    0,    0,   0,    0,  20 } },
};

static bool profile_state_by_name(const char *name, channel_state_t *out)
{
    if (!name || !out) return false;
    for (size_t i = 0; i < sizeof(k_builtin_profiles) / sizeof(k_builtin_profiles[0]); ++i) {
        if (strcmp(k_builtin_profiles[i].name, name) == 0) {
            memcpy(out->values, k_builtin_profiles[i].intensities, sizeof out->values);
            return true;
        }
    }
    config_profiles_t profiles;
    if (config_store_load_profiles(&profiles) != ESP_OK) return false;
    for (uint8_t i = 0; i < profiles.count; ++i) {
        if (strcmp(profiles.profiles[i].name, name) == 0) {
            memcpy(out->values, profiles.profiles[i].intensities, sizeof out->values);
            return true;
        }
    }
    return false;
}

static schedule_runtime_t *runtime_for(const config_schedule_t *s)
{
    for (size_t i = 0; i < MAX_SCHEDULES; ++i) {
        if (s_runtime[i].schedule_id[0] &&
            strcmp(s_runtime[i].schedule_id, s->schedule_id) == 0) {
            return &s_runtime[i];
        }
    }
    for (size_t i = 0; i < MAX_SCHEDULES; ++i) {
        if (!s_runtime[i].schedule_id[0]) {
            strncpy(s_runtime[i].schedule_id, s->schedule_id, sizeof s_runtime[i].schedule_id - 1);
            s_runtime[i].last_intensity = -1;
            s_runtime[i].last_yday = -1;
            return &s_runtime[i];
        }
    }
    return NULL;
}

static void update_next_action(int now_minute, int sunrise, int sunset)
{
    int best_delta = 1441;
    const config_schedule_t *best = NULL;
    bool best_start = true;
    for (uint8_t i = 0; i < s_cfg.count; ++i) {
        const config_schedule_t *s = &s_cfg.schedules[i];
        if (!s->enabled) continue;
        int start = schedule_resolve_trigger_minute(s, true, sunrise, sunset);
        int end = schedule_resolve_trigger_minute(s, false, sunrise, sunset);
        int candidates[2] = { start, end };
        for (int j = 0; j < 2; ++j) {
            if (candidates[j] < 0) continue;
            int delta = candidates[j] - now_minute;
            if (delta < 0) delta += 1440;
            if (delta < best_delta) {
                best_delta = delta;
                best = s;
                best_start = j == 0;
            }
        }
    }
    if (!best) {
        strncpy(s_next_action, "No enabled schedules", sizeof s_next_action - 1);
        s_next_action[sizeof s_next_action - 1] = '\0';
        return;
    }
    int at = wrap_minute(now_minute + best_delta);
    snprintf(s_next_action, sizeof s_next_action, "%s %s at %02d:%02d",
             best->name[0] ? best->name : best->schedule_id,
             best_start ? "starts" : "ends", at / 60, at % 60);
}

static void submit_schedule(const config_schedule_t *s, int intensity)
{
    channel_state_t state;
    if (!profile_state_by_name(s->profile_name, &state)) {
        ESP_LOGW(TAG, "schedule %s profile not found: %s", s->schedule_id, s->profile_name);
        return;
    }
    state.values[0] = (uint16_t)(clamp_percent(intensity) * 10);

    ce_request_t req;
    memset(&req, 0, sizeof req);
    req.source = CMD_SOURCE_SYSTEM;
    req.kind = CE_KIND_SET_CHANNELS;
    req.target_type = (s->target_type == CONFIG_SCHEDULE_TARGET_GROUP)
        ? CE_TARGET_GROUP : CE_TARGET_LIGHT;
    strncpy(req.target_id, s->target_id, sizeof req.target_id - 1);
    req.replace = true;
    req.scene_timeout_sec = 60;
    req.command_timeout_ms = 30000;
    req.channel_count = CHANNEL_COUNT;
    for (size_t i = 0; i < CHANNEL_COUNT; ++i) {
        req.channels[i].name = channel_model_all()[i].name;
        req.channels[i].value = state.values[i];
    }

    char cmd_id[CMD_ID_LEN];
    ce_result_t res = command_engine_submit(&req, cmd_id);
    ESP_LOGI(TAG, "schedule %s intensity=%d result=%d cmd=%s",
             s->schedule_id, intensity, (int)res, cmd_id);
}

static void evaluate_once(void)
{
    time_t now = time(NULL);
    if (now <= 1704067200) {
        strncpy(s_next_action, "Waiting for time sync", sizeof s_next_action - 1);
        return;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    int now_minute = lt.tm_hour * 60 + lt.tm_min;
    int sunrise = -1, sunset = -1;
    sun_service_get_today(&sunrise, &sunset);
    update_next_action(now_minute, sunrise, sunset);

    for (uint8_t i = 0; i < s_cfg.count; ++i) {
        config_schedule_t *s = &s_cfg.schedules[i];
        if (!s->enabled || !s->schedule_id[0] || !s->target_id[0] || !s->profile_name[0]) continue;
        int start = schedule_resolve_trigger_minute(s, true, sunrise, sunset);
        int end = schedule_resolve_trigger_minute(s, false, sunrise, sunset);
        int intensity = schedule_active_intensity_percent(s, now_minute, start, end);
        if (intensity < 0) continue;
        schedule_runtime_t *rt = runtime_for(s);
        if (!rt) continue;
        if (rt->last_yday != lt.tm_yday) {
            rt->last_yday = lt.tm_yday;
            rt->last_intensity = -1;
        }
        if (rt->last_intensity == intensity) continue;
        submit_schedule(s, intensity);
        rt->last_intensity = intensity;
    }
}

static void schedule_task(void *arg)
{
    (void)arg;
    for (;;) {
        evaluate_once();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t schedule_engine_init(void)
{
    config_store_load_schedules(&s_cfg);
    memset(s_runtime, 0, sizeof s_runtime);
    for (size_t i = 0; i < MAX_SCHEDULES; ++i) {
        s_runtime[i].last_intensity = -1;
        s_runtime[i].last_yday = -1;
    }
    if (!s_task) {
        BaseType_t ok = xTaskCreate(schedule_task, "schedule", 4096, NULL, 4, &s_task);
        if (ok != pdPASS) return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "init schedules=%u", (unsigned)s_cfg.count);
    return ESP_OK;
}

esp_err_t schedule_engine_reconfigure(void)
{
    config_store_load_schedules(&s_cfg);
    memset(s_runtime, 0, sizeof s_runtime);
    for (size_t i = 0; i < MAX_SCHEDULES; ++i) {
        s_runtime[i].last_intensity = -1;
        s_runtime[i].last_yday = -1;
    }
    ESP_LOGI(TAG, "reconfigure schedules=%u", (unsigned)s_cfg.count);
    return ESP_OK;
}

void schedule_engine_get_status(schedule_engine_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->running = s_running;
    out->schedule_count = s_cfg.count;
    strncpy(out->next_action, s_next_action, sizeof out->next_action - 1);
}

#endif /* ESP_PLATFORM */
