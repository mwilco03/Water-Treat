#ifndef SENSOR_INSTANCE_H
#define SENSOR_INSTANCE_H

#include "common.h"
#include "db/database.h"
#include "db/db_modules.h"
#include "formula_evaluator.h"
#include <pthread.h>
#include "sensors/drivers/driver_web_poll.h"

typedef enum {
    SENSOR_INSTANCE_PHYSICAL,
    SENSOR_INSTANCE_ADC,
    SENSOR_INSTANCE_CALCULATED,
    SENSOR_INSTANCE_WEB_POLL,
    SENSOR_INSTANCE_STATIC
} sensor_instance_type_t;

typedef enum {
    PHYSICAL_DRIVER_NONE = 0,
    PHYSICAL_DRIVER_DS18B20,
    PHYSICAL_DRIVER_DHT22,
    ADC_DRIVER_ADS1115,
    ADC_DRIVER_MCP3008
} sensor_driver_type_t;

/* Union of all driver context structures */
typedef union {
    void *generic;
    struct {
        char device_id[20];
        char device_path[128];
        bool initialized;
        float last_temp;
        uint64_t last_read_time;
    } ds18b20;
    struct {
        int gpio_pin;
        bool initialized;
        float last_temperature;
        float last_humidity;
        uint64_t last_read_time;
    } dht22;
    struct {
        int fd;
        uint8_t address;
        int bus;
        int pga;
        bool initialized;
    } ads1115;
    struct {
        int fd;
        int bus;
        int device;
        float vref;
        bool initialized;
    } mcp3008;

    web_poll_device_t web_poll;

} sensor_driver_ctx_t;

typedef struct {
    int id;
    int module_id;
    int slot;
    char name[MAX_NAME_LEN];
    sensor_instance_type_t type;
    sensor_driver_type_t driver_type;  // Specific driver for cleanup

    void *driver_handle;
    void *driver_ctx;
    sensor_driver_ctx_t driver;

    pthread_mutex_t mutex;

    float current_value;
    int32_t current_raw_value;
    char status[16];
    uint64_t last_read_ms;  // Timestamp in milliseconds (from get_time_ms())
    int poll_rate_ms;
    int timeout_ms;

    /* Calibration settings */
    float cal_scale;
    float cal_offset;
    int32_t raw_min;
    int32_t raw_max;
    float eng_min;
    float eng_max;
    float offset;
    float scale_factor;

    /* Moving average filter */
    bool enable_moving_avg;
    int moving_avg_samples;
    float *avg_buffer;
    int avg_index;

    /* Connection tracking */
    bool connected;
    int consecutive_successes;
    int consecutive_failures;

    /* Total counters for health metrics (P2 operator observability) */
    uint64_t total_reads;           /* Total read attempts since startup */
    uint64_t total_failures;        /* Total failed read attempts */

    /* Data quality tracking (per DEVELOPMENT_GUIDELINES.md Part 2.4) */
    data_quality_t quality;         /* Current quality indicator */
    uint64_t timestamp_us;          /* Microseconds since epoch of last reading */
    uint32_t stale_timeout_ms;      /* Max age before UNCERTAIN (default 5000) */
    uint8_t failure_threshold;      /* Failures before BAD (default 3) */
    float range_min;                /* Minimum valid value */
    float range_max;                /* Maximum valid value */

    /* Calculated sensor support */
    char formula[MAX_CONFIG_VALUE_LEN];
    int input_slots[8];
    int input_count;
    formula_evaluator_t formula_eval;  // Compiled formula evaluator
} sensor_instance_t;

result_t sensor_instance_create_from_db(sensor_instance_t *instance, db_module_t *module, database_t *db);
result_t sensor_instance_read(sensor_instance_t *instance, float *value);

/**
 * @brief Read sensor with full quality information
 *
 * Per DEVELOPMENT_GUIDELINES.md Part 2.3, every sensor read produces
 * value + quality + timestamp. This function populates the full
 * sensor_reading_t structure.
 *
 * @param[in]  instance  Sensor instance to read
 * @param[out] reading   Structure to populate with value, quality, timestamp
 * @return RESULT_OK on successful read (quality may still be UNCERTAIN/BAD)
 */
result_t sensor_instance_read_with_quality(sensor_instance_t *instance, sensor_reading_t *reading);

/**
 * @brief Get current quality indicator for sensor
 */
data_quality_t sensor_instance_get_quality(sensor_instance_t *instance);

result_t sensor_instance_test(sensor_instance_t *instance);
void sensor_instance_destroy(sensor_instance_t *instance);
result_t sensor_instance_evaluate_formula(const char *formula,
                                         const float *input_values,
                                         int input_count,
                                         float *result);
result_t sensor_instance_evaluate_calculated(sensor_instance_t *instance,
                                             const float *input_values,
                                             float *result);

#endif
