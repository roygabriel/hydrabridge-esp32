#include "unity.h"
#include "all_tests.h"
#include "pump_registry.h"

#include <string.h>
#include <stdio.h>

static void make_pump(registered_pump_t *out, const char *id, const char *serial,
                      uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5)
{
    memset(out, 0, sizeof *out);
    strncpy(out->pump_id, id, PUMP_ID_LEN - 1);
    strncpy(out->display_name, "test pump", PUMP_NAME_LEN - 1);
    strncpy(out->serial, serial, PUMP_SERIAL_LEN - 1);
    out->ble_addr[0] = a0; out->ble_addr[1] = a1; out->ble_addr[2] = a2;
    out->ble_addr[3] = a3; out->ble_addr[4] = a4; out->ble_addr[5] = a5;
    out->ble_addr_type = BLE_ADDR_PUBLIC;
    out->model = 41;
    out->enabled = true;
    out->last_seen_rssi = -50;
    out->protocol_status = PUMP_PROTOCOL_EXPERIMENTAL_MOBIUS;
}

static void test_pump_registry_starts_empty(void)
{
    pump_registry_reset();
    TEST_ASSERT_EQUAL_size_t(0, pump_registry_count());
    TEST_ASSERT_NULL(pump_registry_at(0));
}

static void test_pump_registry_add_get_and_dup(void)
{
    pump_registry_reset();
    registered_pump_t p;
    make_pump(&p, "pump-1", "P1", 1,2,3,4,5,6);
    TEST_ASSERT_EQUAL_INT(0, pump_registry_add(&p));
    TEST_ASSERT_EQUAL_INT(-1, pump_registry_add(&p));
    TEST_ASSERT_EQUAL_size_t(1, pump_registry_count());
    TEST_ASSERT_EQUAL_STRING("P1", pump_registry_get("pump-1")->serial);
    TEST_ASSERT_EQUAL_STRING("pump-1", pump_registry_get_by_serial("P1")->pump_id);
}

static void test_pump_registry_addr_lookup_and_update(void)
{
    pump_registry_reset();
    registered_pump_t p;
    make_pump(&p, "pump-1", "P1", 1,2,3,4,5,6);
    TEST_ASSERT_EQUAL_INT(0, pump_registry_add(&p));
    uint8_t addr[BLE_ADDR_BYTES] = {1,2,3,4,5,6};
    TEST_ASSERT_EQUAL_STRING("pump-1", pump_registry_get_by_addr(addr)->pump_id);
    uint8_t next[BLE_ADDR_BYTES] = {6,5,4,3,2,1};
    TEST_ASSERT_EQUAL_INT(0, pump_registry_update_discovery("pump-1", next, BLE_ADDR_RANDOM, -42));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(next, pump_registry_get("pump-1")->ble_addr, BLE_ADDR_BYTES);
    TEST_ASSERT_EQUAL_INT(BLE_ADDR_RANDOM, pump_registry_get("pump-1")->ble_addr_type);
    TEST_ASSERT_EQUAL_INT8(-42, pump_registry_get("pump-1")->last_seen_rssi);
}

static void test_pump_registry_status_rename_remove(void)
{
    pump_registry_reset();
    registered_pump_t p;
    make_pump(&p, "pump-1", "P1", 1,2,3,4,5,6);
    TEST_ASSERT_EQUAL_INT(0, pump_registry_add(&p));
    TEST_ASSERT_EQUAL_INT(0, pump_registry_rename("pump-1", "  Orbit Left  "));
    TEST_ASSERT_EQUAL_STRING("Orbit Left", pump_registry_get("pump-1")->display_name);
    TEST_ASSERT_EQUAL_INT(0, pump_registry_set_status("pump-1", PUMP_MODE_FEED, 10));
    TEST_ASSERT_EQUAL_INT(PUMP_MODE_FEED, pump_registry_get("pump-1")->last_mode);
    TEST_ASSERT_EQUAL_UINT8(10, pump_registry_get("pump-1")->last_speed_percent);
    TEST_ASSERT_EQUAL_INT(-1, pump_registry_set_status("pump-1", PUMP_MODE_FEED, 101));
    TEST_ASSERT_EQUAL_INT(0, pump_registry_remove("pump-1"));
    TEST_ASSERT_NULL(pump_registry_get("pump-1"));
}

static void test_pump_registry_capacity(void)
{
    pump_registry_reset();
    registered_pump_t p;
    char id[16];
    for (int i = 0; i < PUMP_REGISTRY_CAPACITY; ++i) {
        snprintf(id, sizeof id, "p-%d", i);
        make_pump(&p, id, id, (uint8_t)i, 0, 0, 0, 0, 0);
        TEST_ASSERT_EQUAL_INT(0, pump_registry_add(&p));
    }
    make_pump(&p, "overflow", "overflow", 9,0,0,0,0,0);
    TEST_ASSERT_EQUAL_INT(-1, pump_registry_add(&p));
}

void register_pump_registry_tests(void)
{
    RUN_TEST(test_pump_registry_starts_empty);
    RUN_TEST(test_pump_registry_add_get_and_dup);
    RUN_TEST(test_pump_registry_addr_lookup_and_update);
    RUN_TEST(test_pump_registry_status_rename_remove);
    RUN_TEST(test_pump_registry_capacity);
}
