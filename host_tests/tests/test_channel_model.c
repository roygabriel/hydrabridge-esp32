/* Phase 1.4: channel_model tests pin canonical channel names, visual
 * IDs, fixture ordering, and the partial-update / replace+merge
 * semantics to the spec.
 *
 * Channel ordering and visual IDs come from the captured
 * SupportedColorChannels response (RE doc line 614):
 *   01 10 11 19 17 15 13 12 1e
 */
#include "unity.h"
#include "all_tests.h"
#include "channel_model.h"

#include <string.h>

/* ---- lookup ---- */

static void test_canonical_order_and_ids(void)
{
    const channel_def_t *all = channel_model_all();
    TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_EQUAL_STRING("brightness", all[0].name); TEST_ASSERT_EQUAL_HEX8(0x01, all[0].visual_id);
    TEST_ASSERT_EQUAL_STRING("coolwhite",  all[1].name); TEST_ASSERT_EQUAL_HEX8(0x10, all[1].visual_id);
    TEST_ASSERT_EQUAL_STRING("blue",       all[2].name); TEST_ASSERT_EQUAL_HEX8(0x11, all[2].visual_id);
    TEST_ASSERT_EQUAL_STRING("deepred",    all[3].name); TEST_ASSERT_EQUAL_HEX8(0x19, all[3].visual_id);
    TEST_ASSERT_EQUAL_STRING("violet",     all[4].name); TEST_ASSERT_EQUAL_HEX8(0x17, all[4].visual_id);
    TEST_ASSERT_EQUAL_STRING("uv",         all[5].name); TEST_ASSERT_EQUAL_HEX8(0x15, all[5].visual_id);
    TEST_ASSERT_EQUAL_STRING("green",      all[6].name); TEST_ASSERT_EQUAL_HEX8(0x13, all[6].visual_id);
    TEST_ASSERT_EQUAL_STRING("royalblue",  all[7].name); TEST_ASSERT_EQUAL_HEX8(0x12, all[7].visual_id);
    TEST_ASSERT_EQUAL_STRING("moonlight",  all[8].name); TEST_ASSERT_EQUAL_HEX8(0x1e, all[8].visual_id);
}

static void test_by_name_known(void)
{
    const channel_def_t *d = channel_model_by_name("brightness");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_HEX8(0x01, d->visual_id);
    TEST_ASSERT_EQUAL_INT(CHANNEL_ROLE_MASTER, d->role);
    TEST_ASSERT_EQUAL_UINT16(0,    d->min);
    TEST_ASSERT_EQUAL_UINT16(1000, d->max);

    d = channel_model_by_name("moonlight");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_HEX8(0x1e, d->visual_id);
    TEST_ASSERT_EQUAL_INT(CHANNEL_ROLE_MOON, d->role);
}

static void test_by_name_unknown(void)
{
    TEST_ASSERT_NULL(channel_model_by_name("nonexistent"));
    TEST_ASSERT_NULL(channel_model_by_name("Brightness")); /* case-sensitive */
    TEST_ASSERT_NULL(channel_model_by_name(""));
    TEST_ASSERT_NULL(channel_model_by_name(NULL));
}

static void test_by_visual_id_known(void)
{
    TEST_ASSERT_EQUAL_STRING("brightness", channel_model_by_visual_id(0x01)->name);
    TEST_ASSERT_EQUAL_STRING("moonlight",  channel_model_by_visual_id(0x1e)->name);
    TEST_ASSERT_EQUAL_STRING("royalblue",  channel_model_by_visual_id(0x12)->name);
}

static void test_by_visual_id_unknown(void)
{
    TEST_ASSERT_NULL(channel_model_by_visual_id(0x00));
    TEST_ASSERT_NULL(channel_model_by_visual_id(0xFF));
    TEST_ASSERT_NULL(channel_model_by_visual_id(0x20)); /* MoonlightBlue not on this fixture */
}

/* ---- value validation ---- */

static void test_value_is_valid(void)
{
    TEST_ASSERT_TRUE(channel_value_is_valid(0));
    TEST_ASSERT_TRUE(channel_value_is_valid(500));
    TEST_ASSERT_TRUE(channel_value_is_valid(1000));
    TEST_ASSERT_FALSE(channel_value_is_valid(1001));
    TEST_ASSERT_FALSE(channel_value_is_valid(0xFFFF));
}

/* ---- state ---- */

static void test_state_zero(void)
{
    channel_state_t s;
    for (int i = 0; i < CHANNEL_COUNT; ++i) s.values[i] = 9999;
    channel_state_zero(&s);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(0, s.values[i]);
    }
}

static void test_state_build_replace_all(void)
{
    channel_kv_t kvs[] = {
        { "brightness", 1000 }, { "coolwhite", 1000 }, { "blue", 1000 },
        { "deepred",    1000 }, { "violet",    1000 }, { "uv",   1000 },
        { "green",      1000 }, { "royalblue", 1000 }, { "moonlight", 1000 },
    };
    channel_state_t out;
    int rc = channel_state_build(&out, NULL, true, kvs, sizeof kvs / sizeof kvs[0]);
    TEST_ASSERT_EQUAL_INT(0, rc);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(1000, out.values[i]);
    }
}

static void test_state_build_replace_partial(void)
{
    /* "moonlight" preset from the spec: Brightness=1000, Moonlight=50,
     * everything else implicitly 0. */
    channel_kv_t kvs[] = {
        { "brightness", 1000 },
        { "moonlight",   50  },
    };
    channel_state_t out;
    int rc = channel_state_build(&out, NULL, true, kvs, 2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(1000, out.values[0]); /* brightness */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[1]); /* coolwhite  */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[2]); /* blue       */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[3]); /* deepred    */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[4]); /* violet     */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[5]); /* uv         */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[6]); /* green      */
    TEST_ASSERT_EQUAL_UINT16(0,    out.values[7]); /* royalblue  */
    TEST_ASSERT_EQUAL_UINT16(50,   out.values[8]); /* moonlight  */
}

static void test_state_build_merge(void)
{
    channel_state_t base;
    for (int i = 0; i < CHANNEL_COUNT; ++i) base.values[i] = 500;

    channel_kv_t kvs[] = {{ "brightness", 1000 }};
    channel_state_t out;
    int rc = channel_state_build(&out, &base, false, kvs, 1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(1000, out.values[0]); /* brightness updated */
    for (int i = 1; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(500, out.values[i]); /* others preserved */
    }
}

static void test_state_build_unknown_name_atomic(void)
{
    channel_state_t out;
    for (int i = 0; i < CHANNEL_COUNT; ++i) out.values[i] = 42;
    channel_state_t snapshot = out;

    channel_kv_t kvs[] = {{ "brightness", 100 }, { "bogus_channel", 50 }};
    int rc = channel_state_build(&out, NULL, true, kvs, 2);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(snapshot.values[i], out.values[i]);
    }
}

static void test_state_build_out_of_range_atomic(void)
{
    channel_state_t out;
    for (int i = 0; i < CHANNEL_COUNT; ++i) out.values[i] = 42;
    channel_state_t snapshot = out;

    channel_kv_t kvs[] = {{ "brightness", 1001 }};
    int rc = channel_state_build(&out, NULL, true, kvs, 1);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(snapshot.values[i], out.values[i]);
    }
}

static void test_state_build_null_args(void)
{
    channel_state_t out;
    TEST_ASSERT_EQUAL_INT(-1, channel_state_build(&out, NULL, false, NULL, 0));
    TEST_ASSERT_EQUAL_INT(-1, channel_state_build(NULL, NULL, true,  NULL, 0));
}

static void test_state_build_empty_kvs_replace_is_zero(void)
{
    channel_state_t out;
    int rc = channel_state_build(&out, NULL, true, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT16(0, out.values[i]);
    }
}

void register_channel_model_tests(void)
{
    RUN_TEST(test_canonical_order_and_ids);
    RUN_TEST(test_by_name_known);
    RUN_TEST(test_by_name_unknown);
    RUN_TEST(test_by_visual_id_known);
    RUN_TEST(test_by_visual_id_unknown);
    RUN_TEST(test_value_is_valid);
    RUN_TEST(test_state_zero);
    RUN_TEST(test_state_build_replace_all);
    RUN_TEST(test_state_build_replace_partial);
    RUN_TEST(test_state_build_merge);
    RUN_TEST(test_state_build_unknown_name_atomic);
    RUN_TEST(test_state_build_out_of_range_atomic);
    RUN_TEST(test_state_build_null_args);
    RUN_TEST(test_state_build_empty_kvs_replace_is_zero);
}
