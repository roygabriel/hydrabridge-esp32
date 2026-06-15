#include "unity.h"
#include "all_tests.h"
#include "schedule_engine.h"

#include <string.h>

static config_schedule_t make_schedule(void)
{
    config_schedule_t s;
    memset(&s, 0, sizeof s);
    s.enabled = true;
    s.start_trigger = CONFIG_SCHEDULE_TRIGGER_FIXED;
    s.end_trigger = CONFIG_SCHEDULE_TRIGGER_FIXED;
    s.start_minute = 8 * 60;
    s.end_minute = 18 * 60;
    s.intensity_percent = 70;
    s.end_intensity_percent = 0;
    s.ramp_up_min = 60;
    s.ramp_down_min = 120;
    return s;
}

static void test_fixed_trigger_resolution(void)
{
    config_schedule_t s = make_schedule();
    TEST_ASSERT_EQUAL_INT(480, schedule_resolve_trigger_minute(&s, true, -1, -1));
    TEST_ASSERT_EQUAL_INT(1080, schedule_resolve_trigger_minute(&s, false, -1, -1));
}

static void test_sun_trigger_offsets_wrap(void)
{
    config_schedule_t s = make_schedule();
    s.start_trigger = CONFIG_SCHEDULE_TRIGGER_SUNRISE;
    s.start_offset_min = -45;
    s.end_trigger = CONFIG_SCHEDULE_TRIGGER_SUNSET;
    s.end_offset_min = 30;
    TEST_ASSERT_EQUAL_INT(315, schedule_resolve_trigger_minute(&s, true, 360, 1080));
    TEST_ASSERT_EQUAL_INT(1110, schedule_resolve_trigger_minute(&s, false, 360, 1080));
}

static void test_schedule_ramps_and_hold(void)
{
    config_schedule_t s = make_schedule();
    TEST_ASSERT_EQUAL_INT(-1, schedule_active_intensity_percent(&s, 7 * 60, 480, 1080));
    TEST_ASSERT_EQUAL_INT(0, schedule_active_intensity_percent(&s, 480, 480, 1080));
    TEST_ASSERT_EQUAL_INT(35, schedule_active_intensity_percent(&s, 510, 480, 1080));
    TEST_ASSERT_EQUAL_INT(70, schedule_active_intensity_percent(&s, 600, 480, 1080));
    TEST_ASSERT_EQUAL_INT(17, schedule_active_intensity_percent(&s, 1050, 480, 1080));
    TEST_ASSERT_EQUAL_INT(0, schedule_active_intensity_percent(&s, 1080, 480, 1080));
}

static void test_overnight_schedule(void)
{
    config_schedule_t s = make_schedule();
    s.start_minute = 22 * 60;
    s.end_minute = 2 * 60;
    s.ramp_up_min = 0;
    s.ramp_down_min = 0;
    TEST_ASSERT_EQUAL_INT(70, schedule_active_intensity_percent(&s, 23 * 60, 1320, 120));
    TEST_ASSERT_EQUAL_INT(70, schedule_active_intensity_percent(&s, 60, 1320, 120));
    TEST_ASSERT_EQUAL_INT(-1, schedule_active_intensity_percent(&s, 12 * 60, 1320, 120));
}

void register_schedule_engine_tests(void)
{
    RUN_TEST(test_fixed_trigger_resolution);
    RUN_TEST(test_sun_trigger_offsets_wrap);
    RUN_TEST(test_schedule_ramps_and_hold);
    RUN_TEST(test_overnight_schedule);
}
