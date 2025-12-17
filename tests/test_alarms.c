/**
 * @file test_alarms.c
 * @brief Unit tests for alarm threshold checking
 */

#include "test_framework.h"
#include <stdbool.h>

/* Alarm priority levels */
typedef enum {
    ALARM_PRIORITY_LOW = 1,
    ALARM_PRIORITY_MEDIUM = 2,
    ALARM_PRIORITY_HIGH = 3,
    ALARM_PRIORITY_CRITICAL = 4
} alarm_priority_t;

/* Simplified alarm configuration for testing */
typedef struct {
    float low_low_threshold;
    float low_threshold;
    float high_threshold;
    float high_high_threshold;
    float hysteresis;
    bool low_low_enabled;
    bool low_enabled;
    bool high_enabled;
    bool high_high_enabled;
} alarm_config_t;

/* Alarm state */
typedef enum {
    ALARM_STATE_NORMAL = 0,
    ALARM_STATE_LOW_LOW,
    ALARM_STATE_LOW,
    ALARM_STATE_HIGH,
    ALARM_STATE_HIGH_HIGH
} alarm_state_t;

/* Check alarm state based on value and thresholds */
static alarm_state_t check_alarm_state(const alarm_config_t *cfg, float value, alarm_state_t current_state) {
    /* Check from most severe to least severe */
    if (cfg->high_high_enabled && value >= cfg->high_high_threshold) {
        return ALARM_STATE_HIGH_HIGH;
    }

    if (cfg->low_low_enabled && value <= cfg->low_low_threshold) {
        return ALARM_STATE_LOW_LOW;
    }

    if (cfg->high_enabled) {
        if (current_state == ALARM_STATE_HIGH) {
            /* Need hysteresis to clear */
            if (value < cfg->high_threshold - cfg->hysteresis) {
                /* Check lower levels */
            } else {
                return ALARM_STATE_HIGH;
            }
        } else if (value >= cfg->high_threshold) {
            return ALARM_STATE_HIGH;
        }
    }

    if (cfg->low_enabled) {
        if (current_state == ALARM_STATE_LOW) {
            /* Need hysteresis to clear */
            if (value > cfg->low_threshold + cfg->hysteresis) {
                /* Normal */
            } else {
                return ALARM_STATE_LOW;
            }
        } else if (value <= cfg->low_threshold) {
            return ALARM_STATE_LOW;
        }
    }

    return ALARM_STATE_NORMAL;
}

/* Test basic high alarm */
void test_alarm_high_basic(void) {
    alarm_config_t cfg = {
        .high_threshold = 80.0f,
        .high_enabled = true,
        .hysteresis = 2.0f
    };

    /* Below threshold - normal */
    alarm_state_t state = check_alarm_state(&cfg, 75.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);

    /* At threshold - alarming */
    state = check_alarm_state(&cfg, 80.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* Above threshold */
    state = check_alarm_state(&cfg, 85.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);
}

/* Test basic low alarm */
void test_alarm_low_basic(void) {
    alarm_config_t cfg = {
        .low_threshold = 20.0f,
        .low_enabled = true,
        .hysteresis = 2.0f
    };

    /* Above threshold - normal */
    alarm_state_t state = check_alarm_state(&cfg, 25.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);

    /* At threshold - alarming */
    state = check_alarm_state(&cfg, 20.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW, state);

    /* Below threshold */
    state = check_alarm_state(&cfg, 15.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW, state);
}

/* Test high-high alarm (critical) */
void test_alarm_high_high(void) {
    alarm_config_t cfg = {
        .high_threshold = 80.0f,
        .high_high_threshold = 90.0f,
        .high_enabled = true,
        .high_high_enabled = true,
        .hysteresis = 2.0f
    };

    /* Between high and high-high */
    alarm_state_t state = check_alarm_state(&cfg, 85.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* At high-high threshold */
    state = check_alarm_state(&cfg, 90.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH_HIGH, state);

    /* Above high-high */
    state = check_alarm_state(&cfg, 95.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH_HIGH, state);
}

/* Test low-low alarm (critical) */
void test_alarm_low_low(void) {
    alarm_config_t cfg = {
        .low_threshold = 20.0f,
        .low_low_threshold = 10.0f,
        .low_enabled = true,
        .low_low_enabled = true,
        .hysteresis = 2.0f
    };

    /* Between low and low-low */
    alarm_state_t state = check_alarm_state(&cfg, 15.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW, state);

    /* At low-low threshold */
    state = check_alarm_state(&cfg, 10.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW_LOW, state);

    /* Below low-low */
    state = check_alarm_state(&cfg, 5.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW_LOW, state);
}

/* Test hysteresis prevents chatter */
void test_alarm_hysteresis(void) {
    alarm_config_t cfg = {
        .high_threshold = 80.0f,
        .high_enabled = true,
        .hysteresis = 5.0f  /* 5 unit hysteresis */
    };

    /* Trip alarm */
    alarm_state_t state = check_alarm_state(&cfg, 80.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* Just below threshold but within hysteresis - still alarming */
    state = check_alarm_state(&cfg, 78.0f, ALARM_STATE_HIGH);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* Below threshold - hysteresis (75) - clears */
    state = check_alarm_state(&cfg, 74.0f, ALARM_STATE_HIGH);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);
}

/* Test disabled alarms */
void test_alarm_disabled(void) {
    alarm_config_t cfg = {
        .high_threshold = 80.0f,
        .low_threshold = 20.0f,
        .high_enabled = false,
        .low_enabled = false,
        .hysteresis = 2.0f
    };

    /* Even exceeding threshold, should be normal if disabled */
    alarm_state_t state = check_alarm_state(&cfg, 100.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);

    state = check_alarm_state(&cfg, 0.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);
}

/* Test pH alarm thresholds (typical water treatment) */
void test_alarm_ph_scenario(void) {
    /* pH should be 6.5-8.5 for drinking water */
    alarm_config_t cfg = {
        .low_low_threshold = 5.0f,
        .low_threshold = 6.5f,
        .high_threshold = 8.5f,
        .high_high_threshold = 10.0f,
        .low_low_enabled = true,
        .low_enabled = true,
        .high_enabled = true,
        .high_high_enabled = true,
        .hysteresis = 0.2f
    };

    /* Normal pH */
    alarm_state_t state = check_alarm_state(&cfg, 7.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);

    /* Slightly acidic */
    state = check_alarm_state(&cfg, 6.3f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW, state);

    /* Very acidic - critical */
    state = check_alarm_state(&cfg, 4.5f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_LOW_LOW, state);

    /* Slightly alkaline */
    state = check_alarm_state(&cfg, 8.7f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* Very alkaline - critical */
    state = check_alarm_state(&cfg, 10.5f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH_HIGH, state);
}

/* Test turbidity alarm (NTU units) */
void test_alarm_turbidity_scenario(void) {
    /* Turbidity < 1 NTU for drinking water */
    alarm_config_t cfg = {
        .high_threshold = 1.0f,
        .high_high_threshold = 4.0f,
        .high_enabled = true,
        .high_high_enabled = true,
        .hysteresis = 0.1f
    };

    /* Clear water */
    alarm_state_t state = check_alarm_state(&cfg, 0.3f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_NORMAL, state);

    /* Slightly turbid */
    state = check_alarm_state(&cfg, 1.5f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH, state);

    /* Very turbid - critical */
    state = check_alarm_state(&cfg, 5.0f, ALARM_STATE_NORMAL);
    TEST_ASSERT_EQ(ALARM_STATE_HIGH_HIGH, state);
}

void run_alarm_tests(void) {
    TEST_SUITE_BEGIN("Alarm Thresholds");

    RUN_TEST(test_alarm_high_basic);
    RUN_TEST(test_alarm_low_basic);
    RUN_TEST(test_alarm_high_high);
    RUN_TEST(test_alarm_low_low);
    RUN_TEST(test_alarm_hysteresis);
    RUN_TEST(test_alarm_disabled);
    RUN_TEST(test_alarm_ph_scenario);
    RUN_TEST(test_alarm_turbidity_scenario);
}
