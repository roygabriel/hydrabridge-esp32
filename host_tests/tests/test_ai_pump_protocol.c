#include "unity.h"
#include "all_tests.h"
#include "ai_pump_protocol.h"

#include <stdint.h>

static void test_build_constant_speed_payload(void)
{
    ai_pump_command_t cmd = { PUMP_MODE_CONSTANT, 50 };
    uint8_t out[AI_PUMP_SET_LDS_PAYLOAD_BYTES];
    size_t n = ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(AI_PUMP_SET_LDS_PAYLOAD_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(0x94, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);
    TEST_ASSERT_EQUAL_HEX8(AI_PUMP_LIVE_DEMO_SCENE_BYTES, out[4]);
    TEST_ASSERT_EQUAL_HEX8(AI_PUMP_PRIMITIVE_PUMP_V1, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[9]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[10]);
    TEST_ASSERT_EQUAL_HEX8(60, out[11]);
    TEST_ASSERT_EQUAL_HEX8(PUMP_MODE_CONSTANT, out[29]);
    TEST_ASSERT_EQUAL_HEX8(0xf4, out[30]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[31]);
}

static void test_build_feed_payload(void)
{
    ai_pump_command_t cmd = { PUMP_MODE_FEED, 10 };
    uint8_t out[AI_PUMP_SET_LDS_PAYLOAD_BYTES];
    size_t n = ai_pump_build_live_demo_scene_nero_write(&cmd, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(AI_PUMP_SET_LDS_PAYLOAD_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(PUMP_MODE_FEED, out[29]);
    TEST_ASSERT_EQUAL_HEX8(100, out[30]);
    TEST_ASSERT_EQUAL_HEX8(0, out[31]);
}

static void test_build_random_payload(void)
{
    ai_pump_command_t cmd = {
        .mode = PUMP_MODE_RANDOM,
        .speed_percent = 70,
        .min_speed_percent = 20,
        .variance_percent = 50,
    };
    uint8_t out[AI_PUMP_SET_LDS_PAYLOAD_BYTES];
    size_t n = ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(AI_PUMP_SET_LDS_PAYLOAD_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(PUMP_MODE_RANDOM, out[29]);
    TEST_ASSERT_EQUAL_HEX8(0xc8, out[30]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[31]);
    TEST_ASSERT_EQUAL_HEX8(0xbc, out[32]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[33]);
    TEST_ASSERT_EQUAL_HEX8(0xf4, out[34]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[35]);
}

static void test_build_pulse_payload(void)
{
    ai_pump_command_t cmd = {
        .mode = PUMP_MODE_PULSE,
        .speed_percent = 60,
        .on_time_ms = 1500,
        .off_time_ms = 2500,
    };
    uint8_t out[AI_PUMP_SET_LDS_PAYLOAD_BYTES];
    size_t n = ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(AI_PUMP_SET_LDS_PAYLOAD_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(PUMP_MODE_PULSE, out[29]);
    TEST_ASSERT_EQUAL_HEX8(0x58, out[30]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[31]);
    TEST_ASSERT_EQUAL_HEX8(0xdc, out[32]);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[33]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[34]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[35]);
    TEST_ASSERT_EQUAL_HEX8(0xc4, out[36]);
    TEST_ASSERT_EQUAL_HEX8(0x09, out[37]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[38]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[39]);
}

static void test_mode_support_helpers(void)
{
    TEST_ASSERT_TRUE(ai_pump_mode_is_supported(PUMP_MODE_REEF_CREST));
    TEST_ASSERT_TRUE(ai_pump_mode_is_supported(PUMP_MODE_RANDOM));
    TEST_ASSERT_TRUE(ai_pump_mode_is_orbit_supported(PUMP_MODE_RANDOM));
    TEST_ASSERT_TRUE(ai_pump_mode_is_orbit_supported(PUMP_MODE_PULSE));
    TEST_ASSERT_FALSE(ai_pump_mode_is_orbit_supported(PUMP_MODE_REEF_CREST));
    TEST_ASSERT_FALSE(ai_pump_mode_is_supported(99));
}

static void test_rejects_invalid_args(void)
{
    ai_pump_command_t cmd = { PUMP_MODE_CONSTANT, 101 };
    uint8_t out[AI_PUMP_SET_LDS_PAYLOAD_BYTES];
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out));
    cmd.speed_percent = 50;
    cmd.mode = PUMP_MODE_UNKNOWN;
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out));
    cmd.mode = PUMP_MODE_PULSE;
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out));
    cmd.mode = PUMP_MODE_SYNC;
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out));
    cmd.mode = PUMP_MODE_CONSTANT;
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, out, sizeof out - 1));
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(NULL, 60, out, sizeof out));
    TEST_ASSERT_EQUAL_size_t(0, ai_pump_build_live_demo_scene_nero_write(&cmd, 60, NULL, sizeof out));
}

void register_ai_pump_protocol_tests(void)
{
    RUN_TEST(test_build_constant_speed_payload);
    RUN_TEST(test_build_feed_payload);
    RUN_TEST(test_build_random_payload);
    RUN_TEST(test_build_pulse_payload);
    RUN_TEST(test_mode_support_helpers);
    RUN_TEST(test_rejects_invalid_args);
}
