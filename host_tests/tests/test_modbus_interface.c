/* Phase 4.2b: modbus_interface host tests for the store + snapshot-on-
 * command_seq dispatch logic. */
#include "unity.h"
#include "all_tests.h"
#include "modbus_interface.h"
#include "modbus_registers.h"
#include "command_engine.h"
#include "command_queue.h"
#include "light_registry.h"
#include "preset_engine.h"

#include <string.h>

static void setup_with_light(void)
{
    light_registry_reset();
    cmd_queue_reset();
    command_engine_reset();
    modbus_store_reset();

    registered_light_t l;
    memset(&l, 0, sizeof l);
    strncpy(l.light_id, "hydra-0", LIGHT_ID_LEN - 1);
    strncpy(l.display_name, "Hydra A", LIGHT_NAME_LEN - 1);
    strncpy(l.serial, "S1", LIGHT_SERIAL_LEN - 1);
    l.model = 335;
    l.enabled = true;
    l.last_seen_rssi = -65;
    light_registry_add(&l);
}

static void test_magic_and_version_after_reset(void)
{
    modbus_store_reset();
    TEST_ASSERT_EQUAL_HEX16(HYDRA_MODBUS_MAGIC_VALUE, modbus_store_get(HYDRA_MODBUS_REG_MAGIC));
    TEST_ASSERT_EQUAL_UINT16(HYDRA_MODBUS_REG_MAP_VERSION_VALUE,
                             modbus_store_get(HYDRA_MODBUS_REG_MAP_VERSION));
}

static void test_read_write_roundtrip(void)
{
    modbus_store_reset();
    uint16_t v = 0xCAFE;
    TEST_ASSERT_EQUAL_INT(0, modbus_store_write(100, 1, &v));
    uint16_t got = 0;
    TEST_ASSERT_EQUAL_INT(0, modbus_store_read(100, 1, &got));
    TEST_ASSERT_EQUAL_HEX16(0xCAFE, got);
}

static void test_out_of_range(void)
{
    uint16_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, modbus_store_read(MODBUS_STORE_REG_COUNT, 1, &v));
    TEST_ASSERT_EQUAL_INT(-1, modbus_store_write(MODBUS_STORE_REG_COUNT - 1, 2, &v));
}

static void test_no_seq_change_no_dispatch(void)
{
    setup_with_light();
    modbus_store_process_pending();
    TEST_ASSERT_EQUAL_size_t(0, cmd_queue_depth("hydra-0"));
}

static void test_invalid_target_no_light_registered(void)
{
    light_registry_reset();
    cmd_queue_reset();
    command_engine_reset();
    modbus_store_reset();

    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    uint16_t code = HYDRA_CMD_CODE_ON;
    uint16_t seq  = 1;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);

    TEST_ASSERT_EQUAL_UINT16(HYDRA_MODBUS_RESULT_INVALID_TARGET,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_RESULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(1,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_COMMAND_SEQ));
}

static void test_power_on_dispatch(void)
{
    setup_with_light();

    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    uint16_t code = HYDRA_CMD_CODE_ON;
    uint16_t seq  = 1;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);

    TEST_ASSERT_EQUAL_size_t(1, cmd_queue_depth("hydra-0"));
    TEST_ASSERT_EQUAL_UINT16(HYDRA_MODBUS_RESULT_ACCEPTED,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_RESULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(1, modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_COMMAND_SEQ));

    pending_command_t cmd;
    cmd_queue_pop("hydra-0", &cmd);
    TEST_ASSERT_EQUAL_INT(CMD_SOURCE_MODBUS, cmd.source);
    TEST_ASSERT_EQUAL_UINT16(1000, cmd.state.values[0]);
}

static void test_duplicate_seq_no_redispatch(void)
{
    setup_with_light();

    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    uint16_t code = HYDRA_CMD_CODE_ON;
    uint16_t seq  = 7;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);
    TEST_ASSERT_EQUAL_size_t(1, cmd_queue_depth("hydra-0"));

    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ, 1, &seq);
    TEST_ASSERT_EQUAL_size_t(1, cmd_queue_depth("hydra-0"));
}

static void test_fc16_batch_write_single_dispatch(void)
{
    setup_with_light();

    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    uint16_t batch[20] = {0};
    batch[0] = 1;
    batch[1] = HYDRA_CMD_CODE_PRESET;
    batch[5] = HYDRA_PRESET_ID_MOONLIGHT;
    TEST_ASSERT_EQUAL_INT(0, modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ, 12, batch));

    TEST_ASSERT_EQUAL_size_t(1, cmd_queue_depth("hydra-0"));

    pending_command_t cmd;
    cmd_queue_pop("hydra-0", &cmd);
    TEST_ASSERT_EQUAL_UINT16(1000, cmd.state.values[0]);
    TEST_ASSERT_EQUAL_UINT16(50,   cmd.state.values[8]);
}

static void test_apply_channels_reads_all_9(void)
{
    setup_with_light();
    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        uint16_t v = (uint16_t)((i + 1) * 100);
        modbus_store_set(base + HYDRA_LIGHT_OFF_CH_BRIGHTNESS + i, v);
    }
    uint16_t code = HYDRA_CMD_CODE_APPLY_CHANNELS;
    uint16_t seq  = 1;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);

    TEST_ASSERT_EQUAL_size_t(1, cmd_queue_depth("hydra-0"));
    pending_command_t cmd;
    cmd_queue_pop("hydra-0", &cmd);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)((i + 1) * 100), cmd.state.values[i]);
    }
}

static void test_ramp_reads_metadata(void)
{
    setup_with_light();
    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    modbus_store_set(base + HYDRA_LIGHT_OFF_RAMP_FROM,            0);
    modbus_store_set(base + HYDRA_LIGHT_OFF_RAMP_TO,              1000);
    modbus_store_set(base + HYDRA_LIGHT_OFF_RAMP_DURATION_MS_LOW, 0x4E20);
    modbus_store_set(base + HYDRA_LIGHT_OFF_RAMP_DURATION_MS_HIGH, 0);
    modbus_store_set(base + HYDRA_LIGHT_OFF_RAMP_STEPS,           20);

    uint16_t code = HYDRA_CMD_CODE_RAMP;
    uint16_t seq  = 1;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);

    pending_command_t cmd;
    cmd_queue_pop("hydra-0", &cmd);
    TEST_ASSERT_EQUAL_INT(CMD_TYPE_RAMP, cmd.type);
    TEST_ASSERT_EQUAL_UINT16(1000, cmd.ramp_to);
    TEST_ASSERT_EQUAL_UINT32(20000, cmd.ramp_duration_ms);
    TEST_ASSERT_EQUAL_UINT16(20, cmd.ramp_steps);
}

static void test_noop_seq_ack(void)
{
    setup_with_light();
    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    uint16_t code = HYDRA_CMD_CODE_NOOP;
    uint16_t seq  = 99;
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_CODE, 1, &code);
    modbus_store_write(base + HYDRA_LIGHT_OFF_COMMAND_SEQ,  1, &seq);

    TEST_ASSERT_EQUAL_UINT16(HYDRA_MODBUS_RESULT_IDLE,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_RESULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(99,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_COMMAND_SEQ));
    TEST_ASSERT_EQUAL_size_t(0, cmd_queue_depth("hydra-0"));
}

static void test_status_mirror_refresh(void)
{
    setup_with_light();
    channel_state_t s;
    for (int i = 0; i < CHANNEL_COUNT; ++i) s.values[i] = (uint16_t)((i + 1) * 50);
    light_registry_set_last_state("hydra-0", &s);
    light_registry_set_rssi("hydra-0", -72);

    modbus_store_refresh_status_mirrors();

    uint16_t base = HYDRA_MODBUS_LIGHT_BASE(0);
    TEST_ASSERT_EQUAL_UINT16(1, modbus_store_get(base + HYDRA_LIGHT_OFF_PRESENT));
    TEST_ASSERT_EQUAL_UINT16(1, modbus_store_get(base + HYDRA_LIGHT_OFF_ENABLED));
    TEST_ASSERT_EQUAL_UINT16(72, modbus_store_get(base + HYDRA_LIGHT_OFF_LAST_SEEN_RSSI_ABS));
    TEST_ASSERT_EQUAL_UINT16(HYDRA_LIGHT_POWER_ON,
                             modbus_store_get(base + HYDRA_LIGHT_OFF_CURRENT_POWER));
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)((i + 1) * 50),
                                 modbus_store_get(base + HYDRA_LIGHT_OFF_CURRENT_BRIGHTNESS + i));
    }
    TEST_ASSERT_EQUAL_UINT16(1, modbus_store_get(HYDRA_MODBUS_REG_REGISTERED_LIGHT_COUNT));
}

static void test_light_count_zero_when_empty(void)
{
    light_registry_reset();
    modbus_store_reset();
    modbus_store_refresh_status_mirrors();
    TEST_ASSERT_EQUAL_UINT16(0, modbus_store_get(HYDRA_MODBUS_REG_REGISTERED_LIGHT_COUNT));
    TEST_ASSERT_EQUAL_UINT16(0, modbus_store_get(HYDRA_MODBUS_LIGHT_BASE(0) + HYDRA_LIGHT_OFF_PRESENT));
}

void register_modbus_interface_tests(void)
{
    RUN_TEST(test_magic_and_version_after_reset);
    RUN_TEST(test_read_write_roundtrip);
    RUN_TEST(test_out_of_range);
    RUN_TEST(test_no_seq_change_no_dispatch);
    RUN_TEST(test_invalid_target_no_light_registered);
    RUN_TEST(test_power_on_dispatch);
    RUN_TEST(test_duplicate_seq_no_redispatch);
    RUN_TEST(test_fc16_batch_write_single_dispatch);
    RUN_TEST(test_apply_channels_reads_all_9);
    RUN_TEST(test_ramp_reads_metadata);
    RUN_TEST(test_noop_seq_ack);
    RUN_TEST(test_status_mirror_refresh);
    RUN_TEST(test_light_count_zero_when_empty);
}
