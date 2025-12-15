/**
 * @file analog_sensor.c
 * @brief Unified Analog Sensor Driver Implementation
 *
 * This single driver handles pH, TDS, turbidity, pressure, and any other
 * sensor that outputs an analog voltage. The behavior is controlled entirely
 * by configuration and calibration data.
 */

#include "analog_sensor.h"
#include "utils/logger.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Predefined Calibration Presets
 * ========================================================================== */

// pH probe: Linear approximation for typical probe
// Most pH probes: ~0V at pH 0, ~2.5V at pH 7, ~5V at pH 14 (with amplifier)
const sensor_calibration_t CAL_PRESET_PH_GENERIC = {
    .type = CAL_TYPE_LINEAR,
    .linear = {
        .scale = 3.5f,      // pH per volt (typical with amplifier board)
        .offset = 0.0f
    }
};

// TDS probe: Typical analog TDS meter
// Output voltage correlates to conductivity -> TDS
const sensor_calibration_t CAL_PRESET_TDS_GENERIC = {
    .type = CAL_TYPE_POLYNOMIAL,
    .polynomial = {
        .coefficients = {0.0f, 133.42f, 255.86f, -189.34f, 0.0f, 0.0f},
        .degree = 3
    }
};

// Turbidity: Typical analog turbidity sensor
// Higher turbidity = lower voltage (light blocked)
const sensor_calibration_t CAL_PRESET_TURBIDITY_GENERIC = {
    .type = CAL_TYPE_LINEAR,
    .linear = {
        .scale = -1120.4f,  // NTU per volt (inverted - more turbid = less light)
        .offset = 5000.0f
    }
};

// Pressure transducer: 0.5V = 0 PSI, 4.5V = 100 PSI
const sensor_calibration_t CAL_PRESET_PRESSURE_100PSI = {
    .type = CAL_TYPE_LINEAR,
    .linear = {
        .scale = 25.0f,     // PSI per volt
        .offset = -12.5f    // 0.5V offset
    }
};

// 4-20mA transmitter: 4mA = 0%, 20mA = 100%
// Assuming 250 ohm sense resistor: 1V = 4mA, 5V = 20mA
const sensor_calibration_t CAL_PRESET_4_20MA = {
    .type = CAL_TYPE_LINEAR,
    .linear = {
        .scale = 25.0f,     // % per volt
        .offset = -25.0f    // 1V = 0%
    }
};

/* ============================================================================
 * Private Data Structure
 * ========================================================================== */

typedef struct {
    adc_backend_t *adc;
    int channel;
    int gain;

    // Calibration working data
    float cal_raw_low;
    float cal_raw_high;
    bool cal_low_set;
    bool cal_high_set;

    // Filtering
    float filter_alpha;
    float filtered_value;
    bool filter_initialized;

} analog_sensor_priv_t;

/* ============================================================================
 * Calibration Functions
 * ========================================================================== */

float sensor_apply_calibration(const sensor_calibration_t *cal, float raw) {
    if (!cal) return raw;

    switch (cal->type) {
        case CAL_TYPE_NONE:
            return raw;

        case CAL_TYPE_LINEAR:
            return cal->linear.scale * raw + cal->linear.offset;

        case CAL_TYPE_TWO_POINT: {
            // Linear interpolation between two calibration points
            float range_raw = cal->two_point.raw_high - cal->two_point.raw_low;
            float range_ref = cal->two_point.ref_high - cal->two_point.ref_low;
            if (fabsf(range_raw) < 0.0001f) return raw;
            float slope = range_ref / range_raw;
            return cal->two_point.ref_low + slope * (raw - cal->two_point.raw_low);
        }

        case CAL_TYPE_POLYNOMIAL: {
            float result = cal->polynomial.coefficients[0];
            float x_power = 1.0f;
            for (int i = 1; i <= cal->polynomial.degree; i++) {
                x_power *= raw;
                result += cal->polynomial.coefficients[i] * x_power;
            }
            return result;
        }

        case CAL_TYPE_LOOKUP_TABLE: {
            // Linear interpolation in lookup table
            int n = cal->lookup.count;
            if (n < 2) return raw;

            // Find bracketing points
            for (int i = 0; i < n - 1; i++) {
                if (raw >= cal->lookup.raw[i] && raw <= cal->lookup.raw[i + 1]) {
                    float t = (raw - cal->lookup.raw[i]) /
                              (cal->lookup.raw[i + 1] - cal->lookup.raw[i]);
                    return cal->lookup.eng[i] + t * (cal->lookup.eng[i + 1] - cal->lookup.eng[i]);
                }
            }
            // Extrapolate if outside range
            if (raw < cal->lookup.raw[0]) {
                return cal->lookup.eng[0];
            }
            return cal->lookup.eng[n - 1];
        }

        case CAL_TYPE_STEINHART_HART: {
            // For NTC thermistors
            // R = series_resistor * raw / (vref - raw)
            // 1/T = a + b*ln(R) + c*ln(R)^3
            float vref = 3.3f;  // Assume 3.3V reference
            if (raw >= vref) return -273.15f;  // Invalid

            float r = cal->steinhart.series_resistor * raw / (vref - raw);
            if (r <= 0) return -273.15f;

            float ln_r = logf(r);
            float inv_t = cal->steinhart.a +
                          cal->steinhart.b * ln_r +
                          cal->steinhart.c * ln_r * ln_r * ln_r;

            return (1.0f / inv_t) - 273.15f;  // Convert K to C
        }

        default:
            return raw;
    }
}

/* ============================================================================
 * Driver Operations
 * ========================================================================== */

static result_t analog_init(sensor_driver_t *drv, const sensor_config_t *cfg) {
    analog_sensor_priv_t *priv = calloc(1, sizeof(analog_sensor_priv_t));
    if (!priv) return RESULT_NO_MEMORY;

    priv->channel = cfg->hw.adc.adc_channel;
    priv->gain = cfg->hw.adc.adc_gain;
    priv->filter_alpha = cfg->filter_alpha;
    priv->filter_initialized = false;

    drv->priv = priv;

    LOG_DEBUG("Analog sensor '%s' initialized on channel %d",
              cfg->name, priv->channel);

    return RESULT_OK;
}

static result_t analog_read(sensor_driver_t *drv, sensor_reading_t *reading) {
    analog_sensor_priv_t *priv = drv->priv;

    if (!priv->adc) {
        reading->status = SENSOR_STATUS_ERROR;
        return RESULT_NOT_INITIALIZED;
    }

    // Read voltage from ADC
    float voltage;
    result_t r = priv->adc->ops->read_voltage(priv->adc, priv->channel, &voltage);
    if (r != RESULT_OK) {
        reading->status = SENSOR_STATUS_ERROR;
        return r;
    }

    reading->raw_value = voltage;
    reading->timestamp_ms = get_time_ms();

    // Apply calibration
    float calibrated = sensor_apply_calibration(&drv->config.calibration, voltage);

    // Apply EMA filter if configured
    if (priv->filter_alpha > 0.0f && priv->filter_alpha < 1.0f) {
        if (!priv->filter_initialized) {
            priv->filtered_value = calibrated;
            priv->filter_initialized = true;
        } else {
            priv->filtered_value = priv->filter_alpha * calibrated +
                                   (1.0f - priv->filter_alpha) * priv->filtered_value;
        }
        calibrated = priv->filtered_value;
    }

    reading->value = calibrated;

    // Range check
    if (drv->config.range_min != drv->config.range_max) {
        if (calibrated < drv->config.range_min || calibrated > drv->config.range_max) {
            reading->status = SENSOR_STATUS_OUT_OF_RANGE;
        } else {
            reading->status = SENSOR_STATUS_OK;
        }
    } else {
        reading->status = SENSOR_STATUS_OK;
    }

    // Cache the reading
    memcpy(&drv->last_reading, reading, sizeof(sensor_reading_t));

    return RESULT_OK;
}

static result_t analog_calibrate(sensor_driver_t *drv, const sensor_calibration_t *cal) {
    if (!cal) return RESULT_INVALID_PARAM;
    memcpy(&drv->config.calibration, cal, sizeof(sensor_calibration_t));
    LOG_INFO("Sensor '%s' calibration updated (type=%d)", drv->config.name, cal->type);
    return RESULT_OK;
}

static void analog_destroy(sensor_driver_t *drv) {
    if (drv && drv->priv) {
        free(drv->priv);
        drv->priv = NULL;
    }
}

static const sensor_ops_t analog_ops = {
    .init = analog_init,
    .read = analog_read,
    .write = NULL,  // Analog sensors are read-only
    .calibrate = analog_calibrate,
    .destroy = analog_destroy,
};

/* ============================================================================
 * Public API
 * ========================================================================== */

result_t analog_sensor_create(sensor_driver_t **drv,
                               const sensor_config_t *cfg,
                               adc_backend_t *adc) {
    CHECK_NULL(drv);
    CHECK_NULL(cfg);

    sensor_driver_t *d = calloc(1, sizeof(sensor_driver_t));
    if (!d) return RESULT_NO_MEMORY;

    memcpy(&d->config, cfg, sizeof(sensor_config_t));
    d->ops = &analog_ops;

    result_t r = d->ops->init(d, cfg);
    if (r != RESULT_OK) {
        free(d);
        return r;
    }

    // Attach ADC backend
    if (adc) {
        analog_sensor_priv_t *priv = d->priv;
        priv->adc = adc;
    }

    *drv = d;
    return RESULT_OK;
}

result_t analog_sensor_set_adc(sensor_driver_t *drv, adc_backend_t *adc) {
    CHECK_NULL(drv);
    CHECK_NULL(adc);

    analog_sensor_priv_t *priv = drv->priv;
    priv->adc = adc;
    return RESULT_OK;
}

result_t analog_sensor_cal_point(sensor_driver_t *drv,
                                  float reference_value,
                                  bool is_high_point) {
    CHECK_NULL(drv);
    analog_sensor_priv_t *priv = drv->priv;

    // Read current raw value
    sensor_reading_t reading;
    result_t r = analog_read(drv, &reading);
    if (r != RESULT_OK) return r;

    if (is_high_point) {
        priv->cal_raw_high = reading.raw_value;
        priv->cal_high_set = true;
        drv->config.calibration.two_point.raw_high = reading.raw_value;
        drv->config.calibration.two_point.ref_high = reference_value;
    } else {
        priv->cal_raw_low = reading.raw_value;
        priv->cal_low_set = true;
        drv->config.calibration.two_point.raw_low = reading.raw_value;
        drv->config.calibration.two_point.ref_low = reference_value;
    }

    // If both points set, activate two-point calibration
    if (priv->cal_low_set && priv->cal_high_set) {
        drv->config.calibration.type = CAL_TYPE_TWO_POINT;
        LOG_INFO("Sensor '%s' two-point calibration complete: "
                 "raw [%.3f, %.3f] -> ref [%.2f, %.2f]",
                 drv->config.name,
                 drv->config.calibration.two_point.raw_low,
                 drv->config.calibration.two_point.raw_high,
                 drv->config.calibration.two_point.ref_low,
                 drv->config.calibration.two_point.ref_high);
    }

    return RESULT_OK;
}

result_t analog_sensor_read_raw(sensor_driver_t *drv, int *raw_value) {
    CHECK_NULL(drv);
    CHECK_NULL(raw_value);

    analog_sensor_priv_t *priv = drv->priv;
    if (!priv->adc) return RESULT_NOT_INITIALIZED;

    return priv->adc->ops->read_raw(priv->adc, priv->channel, raw_value);
}

result_t analog_sensor_read_voltage(sensor_driver_t *drv, float *voltage) {
    CHECK_NULL(drv);
    CHECK_NULL(voltage);

    analog_sensor_priv_t *priv = drv->priv;
    if (!priv->adc) return RESULT_NOT_INITIALIZED;

    return priv->adc->ops->read_voltage(priv->adc, priv->channel, voltage);
}

result_t analog_sensor_factory(sensor_driver_t **drv, const sensor_config_t *cfg) {
    // Factory creates sensor without ADC - must be attached later
    return analog_sensor_create(drv, cfg, NULL);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

const char* sensor_channel_name(sensor_channel_t chan) {
    switch (chan) {
        case SENSOR_CHAN_VOLTAGE:      return "Voltage";
        case SENSOR_CHAN_CURRENT:      return "Current";
        case SENSOR_CHAN_PH:           return "pH";
        case SENSOR_CHAN_TDS:          return "TDS";
        case SENSOR_CHAN_CONDUCTIVITY: return "Conductivity";
        case SENSOR_CHAN_TURBIDITY:    return "Turbidity";
        case SENSOR_CHAN_ORP:          return "ORP";
        case SENSOR_CHAN_DISSOLVED_O2: return "Dissolved O2";
        case SENSOR_CHAN_PRESSURE:     return "Pressure";
        case SENSOR_CHAN_LEVEL:        return "Level";
        case SENSOR_CHAN_TEMPERATURE:  return "Temperature";
        case SENSOR_CHAN_HUMIDITY:     return "Humidity";
        case SENSOR_CHAN_DISTANCE:     return "Distance";
        case SENSOR_CHAN_WEIGHT:       return "Weight";
        case SENSOR_CHAN_BINARY:       return "Binary";
        default:                       return "Unknown";
    }
}

const char* sensor_status_string(sensor_status_t status) {
    switch (status) {
        case SENSOR_STATUS_OK:           return "OK";
        case SENSOR_STATUS_TIMEOUT:      return "Timeout";
        case SENSOR_STATUS_ERROR:        return "Error";
        case SENSOR_STATUS_OUT_OF_RANGE: return "Out of Range";
        case SENSOR_STATUS_NOT_READY:    return "Not Ready";
        case SENSOR_STATUS_UNCALIBRATED: return "Uncalibrated";
        default:                         return "Unknown";
    }
}
