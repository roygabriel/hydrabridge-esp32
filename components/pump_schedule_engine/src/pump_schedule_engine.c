#include "pump_schedule_engine.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int wrap_minute(int minute)
{
    while (minute < 0) minute += 1440;
    while (minute >= 1440) minute -= 1440;
    return minute;
}

static int minute_delta_forward(int from, int to)
{
    int d = to - from;
    if (d < 0) d += 1440;
    return d;
}

int pump_schedule_resolve_trigger_minute(const config_pump_schedule_t *s,
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

pump_schedule_phase_t pump_schedule_due_phase(const config_pump_schedule_t *s,
                                              int previous_minute,
                                              int now_minute,
                                              int start_minute,
                                              int end_minute)
{
    if (!s || !s->enabled || previous_minute < 0 || now_minute < 0 ||
        start_minute < 0 || end_minute < 0) {
        return PUMP_SCHEDULE_PHASE_NONE;
    }
    int elapsed = minute_delta_forward(previous_minute, now_minute);
    if (elapsed == 0) return PUMP_SCHEDULE_PHASE_NONE;
    int start_delta = minute_delta_forward(previous_minute, start_minute);
    int end_delta = minute_delta_forward(previous_minute, end_minute);
    if (start_delta > 0 && start_delta <= elapsed) return PUMP_SCHEDULE_PHASE_START;
    if (end_delta > 0 && end_delta <= elapsed) return PUMP_SCHEDULE_PHASE_END;
    return PUMP_SCHEDULE_PHASE_NONE;
}

#ifdef ESP_PLATFORM

#include "ai_pump_protocol.h"
#include "command_queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pump_registry.h"
#include "sun_service.h"

static const char *TAG = "pump_schedule";

typedef struct {
    char schedule_id[CONFIG_SCHEDULE_ID_LEN];
    int  last_yday;
    int  last_minute;
} pump_schedule_runtime_t;

static config_pump_schedules_t s_cfg;
static pump_schedule_runtime_t s_runtime[MAX_PUMP_SCHEDULES];
static TaskHandle_t s_task;
static bool s_running;
static char s_next_action[128] = "No pump schedules";

static pump_schedule_runtime_t *runtime_for(const config_pump_schedule_t *s)
{
    for (size_t i = 0; i < MAX_PUMP_SCHEDULES; ++i) {
        if (s_runtime[i].schedule_id[0] &&
            strcmp(s_runtime[i].schedule_id, s->schedule_id) == 0) {
            return &s_runtime[i];
        }
    }
    for (size_t i = 0; i < MAX_PUMP_SCHEDULES; ++i) {
        if (!s_runtime[i].schedule_id[0]) {
            strncpy(s_runtime[i].schedule_id, s->schedule_id, sizeof s_runtime[i].schedule_id - 1);
            s_runtime[i].last_yday = -1;
            s_runtime[i].last_minute = -1;
            return &s_runtime[i];
        }
    }
    return NULL;
}

static void update_next_action(int now_minute, int sunrise, int sunset)
{
    int best_delta = 1441;
    const config_pump_schedule_t *best = NULL;
    bool best_start = true;
    for (uint8_t i = 0; i < s_cfg.count; ++i) {
        const config_pump_schedule_t *s = &s_cfg.schedules[i];
        if (!s->enabled) continue;
        int start = pump_schedule_resolve_trigger_minute(s, true, sunrise, sunset);
        int end = pump_schedule_resolve_trigger_minute(s, false, sunrise, sunset);
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
        strncpy(s_next_action, "No enabled pump schedules", sizeof s_next_action - 1);
        s_next_action[sizeof s_next_action - 1] = '\0';
        return;
    }
    int at = wrap_minute(now_minute + best_delta);
    snprintf(s_next_action, sizeof s_next_action, "%s %s at %02d:%02d",
             best->name[0] ? best->name : best->schedule_id,
             best_start ? "starts" : "ends", at / 60, at % 60);
}

static void submit_pump_schedule(const config_pump_schedule_t *s, pump_schedule_phase_t phase)
{
    uint8_t mode = phase == PUMP_SCHEDULE_PHASE_START ? s->active_mode : s->end_mode;
    uint8_t speed = phase == PUMP_SCHEDULE_PHASE_START ? s->active_speed_percent : s->end_speed_percent;
    uint8_t min_speed = phase == PUMP_SCHEDULE_PHASE_START ? s->active_min_speed_percent : s->end_min_speed_percent;
    uint8_t variance = phase == PUMP_SCHEDULE_PHASE_START ? s->active_variance_percent : s->end_variance_percent;
    uint16_t on_ms = phase == PUMP_SCHEDULE_PHASE_START ? s->active_on_time_ms : s->end_on_time_ms;
    uint16_t off_ms = phase == PUMP_SCHEDULE_PHASE_START ? s->active_off_time_ms : s->end_off_time_ms;
    if (speed > 100 || min_speed > 100 || variance > 100) return;
    if (!ai_pump_mode_is_orbit_supported(mode) || mode == CONFIG_PUMP_MODE_SYNC) return;
    const registered_pump_t *pump = pump_registry_get(s->target_id);
    if (!pump || !pump->enabled) return;

    pending_command_t cmd;
    memset(&cmd, 0, sizeof cmd);
    snprintf(cmd.command_id, sizeof cmd.command_id, "ps-%s", s->schedule_id);
    cmd.source = CMD_SOURCE_SYSTEM;
    cmd.type = CMD_TYPE_PUMP_SET;
    cmd.device_type = CMD_DEVICE_PUMP;
    strncpy(cmd.light_id, s->target_id, sizeof cmd.light_id - 1);
    cmd.timeout_ms = 30000;
    cmd.scene_timeout_sec = 60;
    cmd.pump_mode = mode;
    cmd.pump_speed_percent = speed;
    cmd.pump_min_speed_percent = min_speed;
    cmd.pump_variance_percent = variance;
    cmd.pump_on_time_ms = on_ms;
    cmd.pump_off_time_ms = off_ms;

    int rc = cmd_queue_push(&cmd);
    ESP_LOGI(TAG, "pump schedule %s phase=%d mode=%u speed=%u min=%u variance=%u rc=%d",
             s->schedule_id, (int)phase, mode, speed, min_speed, variance, rc);
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
        config_pump_schedule_t *s = &s_cfg.schedules[i];
        if (!s->enabled || !s->schedule_id[0] || !s->target_id[0]) continue;
        int start = pump_schedule_resolve_trigger_minute(s, true, sunrise, sunset);
        int end = pump_schedule_resolve_trigger_minute(s, false, sunrise, sunset);
        pump_schedule_runtime_t *rt = runtime_for(s);
        if (!rt) continue;
        if (rt->last_yday != lt.tm_yday) {
            rt->last_yday = lt.tm_yday;
            rt->last_minute = now_minute;
            continue;
        }
        pump_schedule_phase_t phase =
            pump_schedule_due_phase(s, rt->last_minute, now_minute, start, end);
        rt->last_minute = now_minute;
        if (phase != PUMP_SCHEDULE_PHASE_NONE) submit_pump_schedule(s, phase);
    }
}

static void pump_schedule_task(void *arg)
{
    (void)arg;
    for (;;) {
        evaluate_once();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t pump_schedule_engine_init(void)
{
    config_store_load_pump_schedules(&s_cfg);
    memset(s_runtime, 0, sizeof s_runtime);
    for (size_t i = 0; i < MAX_PUMP_SCHEDULES; ++i) {
        s_runtime[i].last_yday = -1;
        s_runtime[i].last_minute = -1;
    }
    if (!s_task) {
        BaseType_t ok = xTaskCreate(pump_schedule_task, "pump_sched", 4096, NULL, 4, &s_task);
        if (ok != pdPASS) return ESP_ERR_NO_MEM;
    }
    s_running = true;
    ESP_LOGI(TAG, "init schedules=%u", (unsigned)s_cfg.count);
    return ESP_OK;
}

esp_err_t pump_schedule_engine_reconfigure(void)
{
    config_store_load_pump_schedules(&s_cfg);
    memset(s_runtime, 0, sizeof s_runtime);
    for (size_t i = 0; i < MAX_PUMP_SCHEDULES; ++i) {
        s_runtime[i].last_yday = -1;
        s_runtime[i].last_minute = -1;
    }
    ESP_LOGI(TAG, "reconfigure schedules=%u", (unsigned)s_cfg.count);
    return ESP_OK;
}

void pump_schedule_engine_get_status(pump_schedule_engine_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->running = s_running;
    out->schedule_count = s_cfg.count;
    strncpy(out->next_action, s_next_action, sizeof out->next_action - 1);
}

#endif /* ESP_PLATFORM */
