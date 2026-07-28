#include <kernel/timer.h>
#include <stdio.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #condition);       \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static void test_clock_and_flag_validation(void)
{
    CHECK(timer_clock_sleep_supported(TIMER_CLOCK_REALTIME, 0));
    CHECK(timer_clock_sleep_supported(TIMER_CLOCK_MONOTONIC, TIMER_ABSTIME));
    CHECK(timer_clock_sleep_supported(TIMER_CLOCK_BOOTTIME, 0));
    CHECK(!timer_clock_sleep_supported(2, 0));
    CHECK(!timer_clock_sleep_supported(TIMER_CLOCK_MONOTONIC, 2));
    CHECK(!timer_clock_sleep_supported(TIMER_CLOCK_MONOTONIC, TIMER_ABSTIME | 2));
}

static void test_strict_timespec_validation(void)
{
    uint64_t ns = 123;
    timer_timespec_t negative_sec  = {-1, 0};
    timer_timespec_t negative_nsec = {0, -1};
    timer_timespec_t large_nsec    = {0, 1000000000};
    timer_timespec_t valid         = {12, 345};
    timer_timespec_t overflow      = {__INT64_MAX__, 999999999};

    CHECK(!timer_timespec_to_ns(NULL, &ns));
    CHECK(!timer_timespec_to_ns(&valid, NULL));
    CHECK(!timer_timespec_to_ns(&negative_sec, &ns));
    CHECK(!timer_timespec_to_ns(&negative_nsec, &ns));
    CHECK(!timer_timespec_to_ns(&large_nsec, &ns));
    CHECK(!timer_timespec_to_ns(&overflow, &ns));
    CHECK(timer_timespec_to_ns(&valid, &ns));
    CHECK(ns == 12000000345ULL);
}

static void test_tick_rounding(void)
{
    CHECK(timer_ns_to_ticks_ceil(0) == 0);
    CHECK(timer_ns_to_ticks_ceil(1) == 1);
    CHECK(timer_ns_to_ticks_ceil(TIMER_TICK_NS) == 1);
    CHECK(timer_ns_to_ticks_ceil(TIMER_TICK_NS + 1) == 2);
    CHECK(timer_ns_to_ticks_ceil(UINT64_MAX) == UINT64_MAX / TIMER_TICK_NS + 1);
}

static void test_relative_and_absolute_duration(void)
{
    timer_timespec_t request = {2, 1};
    uint64_t duration;
    uint64_t ticks;

    CHECK(timer_sleep_duration(&request, 9000000000ULL, false, &duration, &ticks));
    CHECK(duration == 2000000001ULL);
    CHECK(ticks == 201);

    CHECK(timer_sleep_duration(&request, 1000000000ULL, true, &duration, &ticks));
    CHECK(duration == 1000000001ULL);
    CHECK(ticks == 101);

    CHECK(timer_sleep_duration(&request, 2000000001ULL, true, &duration, &ticks));
    CHECK(duration == 0);
    CHECK(ticks == 0);

    CHECK(timer_sleep_duration(&request, UINT64_MAX, true, &duration, &ticks));
    CHECK(duration == 0);
    CHECK(ticks == 0);
}

static void test_remainder_conversion(void)
{
    timer_timespec_t remainder = timer_ns_to_timespec(1987654321ULL);
    CHECK(remainder.tv_sec == 1);
    CHECK(remainder.tv_nsec == 987654321);
}

int main(void)
{
    test_clock_and_flag_validation();
    test_strict_timespec_validation();
    test_tick_rounding();
    test_relative_and_absolute_duration();
    test_remainder_conversion();

    if (failures) {
        printf("clock sleep tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("clock sleep tests: passed\n");
    return 0;
}
