#include "unity.h"
#include "all_tests.h"
#include "pump_schedule_engine.h"

#include <string.h>

static config_pump_schedule_t make_pump_schedule(void)
{
    config_pump_schedule_t s;
    memset(&s, 0, sizeof s);
    s.enabled = true;
    s.start_trigger = CONFIG_SCHEDULE_TRIGGER_FIXED;
    s.end_trigger = CONFIG_SCHEDULE_TRIGGER_FIXED;
    s.start_minute = 8 * 60;
    s.end_minute = 18 * 60;
    s.active_mode = CONFIG_PUMP_MODE_CONSTANT;
    s.active_speed_percent = 40;
    s.end_mode = CONFIG_PUMP_MODE_CONSTANT;
    s.end_speed_percent = 20;
    return s;
}

static void test_pump_fixed_trigger_resolution(void)
{
    config_pump_schedule_t s = make_pump_schedule();
    TEST_ASSERT_EQUAL_INT(480, pump_schedule_resolve_trigger_minute(&s, true, -1, -1));
    TEST_ASSERT_EQUAL_INT(1080, pump_schedule_resolve_trigger_minute(&s, false, -1, -1));
}

static void test_pump_sun_trigger_offsets_wrap(void)
{
    config_pump_schedule_t s = make_pump_schedule();
    s.start_trigger = CONFIG_SCHEDULE_TRIGGER_SUNRISE;
    s.start_offset_min = -45;
    s.end_trigger = CONFIG_SCHEDULE_TRIGGER_SUNSET;
    s.end_offset_min = 30;
    TEST_ASSERT_EQUAL_INT(315, pump_schedule_resolve_trigger_minute(&s, true, 360, 1080));
    TEST_ASSERT_EQUAL_INT(1110, pump_schedule_resolve_trigger_minute(&s, false, 360, 1080));
}

static void test_pump_due_phase(void)
{
    config_pump_schedule_t s = make_pump_schedule();
    TEST_ASSERT_EQUAL_INT(PUMP_SCHEDULE_PHASE_NONE, pump_schedule_due_phase(&s, 470, 479, 480, 1080));
    TEST_ASSERT_EQUAL_INT(PUMP_SCHEDULE_PHASE_START, pump_schedule_due_phase(&s, 479, 480, 480, 1080));
    TEST_ASSERT_EQUAL_INT(PUMP_SCHEDULE_PHASE_END, pump_schedule_due_phase(&s, 1079, 1080, 480, 1080));
    TEST_ASSERT_EQUAL_INT(PUMP_SCHEDULE_PHASE_START, pump_schedule_due_phase(&s, 1439, 1, 0, 120));
}

void register_pump_schedule_engine_tests(void)
{
    RUN_TEST(test_pump_fixed_trigger_resolution);
    RUN_TEST(test_pump_sun_trigger_offsets_wrap);
    RUN_TEST(test_pump_due_phase);
}
