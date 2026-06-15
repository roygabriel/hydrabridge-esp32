#include "unity.h"
#include "all_tests.h"
#include "sun_service.h"

static void test_sun_calc_new_york_summer_reasonable(void)
{
    int sunrise = -1;
    int sunset = -1;
    /* New York City, 2026-06-21. UTC times should be roughly 09:25 and 00:30. */
    TEST_ASSERT_TRUE(sun_calc_utc_minutes(2026, 6, 21, 407128000, -740060000,
                                          &sunrise, &sunset));
    TEST_ASSERT_INT_WITHIN(20, 565, sunrise);
    TEST_ASSERT_INT_WITHIN(25, 30, sunset);
}

static void test_sun_calc_rejects_bad_coordinates(void)
{
    int sunrise = -1;
    int sunset = -1;
    TEST_ASSERT_FALSE(sun_calc_utc_minutes(2026, 6, 21, 910000000, 0,
                                           &sunrise, &sunset));
}

void register_sun_service_tests(void)
{
    RUN_TEST(test_sun_calc_new_york_summer_reasonable);
    RUN_TEST(test_sun_calc_rejects_bad_coordinates);
}
