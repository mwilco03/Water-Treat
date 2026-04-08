/**
 * @file driver_hx711.c
 * @brief HX711 24-bit ADC Load Cell Amplifier driver (GPIO)
 */

#include "common.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>   /* T2-C3: per-thread scheduling, not per-process */

#define HX711_GAIN_128_A    1   // Channel A, gain 128
#define HX711_GAIN_32_B     2   // Channel B, gain 32
#define HX711_GAIN_64_A     3   // Channel A, gain 64

typedef struct {
    int dout_pin;
    int sck_pin;
    volatile uint32_t *gpio_base;
    int mem_fd;
    int gain_pulses;
    float scale;
    int32_t offset;
    bool initialized;
} hx711_t;

static volatile uint32_t *map_gpio(int *fd) {
    *fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (*fd < 0) return NULL;
    return (volatile uint32_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
}

static void set_gpio_input(volatile uint32_t *gpio, int pin) {
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    gpio[reg] &= ~(7 << shift);
}

static void set_gpio_output(volatile uint32_t *gpio, int pin) {
    int reg = pin / 10;
    int shift = (pin % 10) * 3;
    gpio[reg] = (gpio[reg] & ~(7 << shift)) | (1 << shift);
}

static void gpio_write(volatile uint32_t *gpio, int pin, int value) {
    if (value) gpio[7] = 1 << pin;
    else gpio[10] = 1 << pin;
}

static int gpio_read(volatile uint32_t *gpio, int pin) {
    return (gpio[13] & (1 << pin)) ? 1 : 0;
}

static void delay_us(int us) {
    struct timespec ts = {0, us * 1000};
    nanosleep(&ts, NULL);
}

result_t hx711_init(hx711_t *dev, int dout_pin, int sck_pin, int gain) {
    CHECK_NULL(dev);
    
    memset(dev, 0, sizeof(*dev));
    dev->dout_pin = dout_pin;
    dev->sck_pin = sck_pin;
    dev->scale = 1.0f;
    dev->offset = 0;
    
    switch (gain) {
        case HX711_GAIN_128_A: dev->gain_pulses = 25; break;
        case HX711_GAIN_32_B: dev->gain_pulses = 26; break;
        case HX711_GAIN_64_A: dev->gain_pulses = 27; break;
        default: dev->gain_pulses = 25; break;
    }
    
    dev->gpio_base = map_gpio(&dev->mem_fd);
    if (!dev->gpio_base) {
        LOG_ERROR("Failed to map GPIO");
        return RESULT_IO_ERROR;
    }
    
    set_gpio_input(dev->gpio_base, dout_pin);
    set_gpio_output(dev->gpio_base, sck_pin);
    gpio_write(dev->gpio_base, sck_pin, 0);
    
    dev->initialized = true;
    LOG_INFO("HX711 initialized: DOUT=%d, SCK=%d", dout_pin, sck_pin);
    return RESULT_OK;
}

result_t hx711_is_ready(hx711_t *dev, bool *ready) {
    CHECK_NULL(dev);
    CHECK_NULL(ready);
    if (!dev->initialized) return RESULT_NOT_INITIALIZED;
    
    *ready = (gpio_read(dev->gpio_base, dev->dout_pin) == 0);
    return RESULT_OK;
}

result_t hx711_read_raw(hx711_t *dev, int32_t *raw) {
    CHECK_NULL(dev);
    CHECK_NULL(raw);
    if (!dev->initialized) return RESULT_NOT_INITIALIZED;
    
    // Wait for ready (DOUT low)
    int timeout = 100;
    while (gpio_read(dev->gpio_base, dev->dout_pin) && --timeout > 0) {
        usleep(1000);
    }
    if (timeout <= 0) return RESULT_TIMEOUT;
    
    /* T2-C3: elevate THIS thread only (see driver_dht22.c for full
     * justification). pthread_setschedparam targets the caller; the
     * old sched_setscheduler(0, ...) targeted the entire process. */
    int saved_policy = SCHED_OTHER;
    struct sched_param saved_param = {.sched_priority = 0};
    pthread_getschedparam(pthread_self(), &saved_policy, &saved_param);
    struct sched_param sp = {.sched_priority = 50};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        static bool warned = false;
        if (!warned) {
            LOG_WARNING("HX711: pthread_setschedparam(SCHED_FIFO) failed; "
                        "reads may be jittery without CAP_SYS_NICE");
            warned = true;
        }
    }

    int32_t value = 0;
    
    // Read 24 bits
    for (int i = 0; i < 24; i++) {
        gpio_write(dev->gpio_base, dev->sck_pin, 1);
        delay_us(1);
        value = (value << 1) | gpio_read(dev->gpio_base, dev->dout_pin);
        gpio_write(dev->gpio_base, dev->sck_pin, 0);
        delay_us(1);
    }
    
    // Additional pulses for gain setting
    for (int i = 24; i < dev->gain_pulses; i++) {
        gpio_write(dev->gpio_base, dev->sck_pin, 1);
        delay_us(1);
        gpio_write(dev->gpio_base, dev->sck_pin, 0);
        delay_us(1);
    }
    
    /* T2-C3: restore this thread's prior policy without touching others. */
    pthread_setschedparam(pthread_self(), saved_policy, &saved_param);

    // Sign extend 24-bit to 32-bit
    if (value & 0x800000) {
        value |= 0xFF000000;
    }
    
    *raw = value;
    return RESULT_OK;
}

result_t hx711_read(hx711_t *dev, float *value) {
    CHECK_NULL(dev);
    CHECK_NULL(value);
    
    int32_t raw;
    result_t r = hx711_read_raw(dev, &raw);
    if (r != RESULT_OK) return r;
    
    *value = (raw - dev->offset) / dev->scale;
    return RESULT_OK;
}

result_t hx711_read_average(hx711_t *dev, int samples, float *value) {
    CHECK_NULL(dev);
    CHECK_NULL(value);
    if (samples <= 0) return RESULT_INVALID_PARAM;
    
    int64_t sum = 0;
    int valid = 0;
    
    for (int i = 0; i < samples; i++) {
        int32_t raw;
        if (hx711_read_raw(dev, &raw) == RESULT_OK) {
            sum += raw;
            valid++;
        }
        usleep(10000);
    }
    
    if (valid == 0) return RESULT_ERROR;
    
    *value = ((float)(sum / valid) - dev->offset) / dev->scale;
    return RESULT_OK;
}

result_t hx711_tare(hx711_t *dev, int samples) {
    CHECK_NULL(dev);
    if (samples <= 0) samples = 10;
    
    int64_t sum = 0;
    int valid = 0;
    
    for (int i = 0; i < samples; i++) {
        int32_t raw;
        if (hx711_read_raw(dev, &raw) == RESULT_OK) {
            sum += raw;
            valid++;
        }
        usleep(50000);
    }
    
    if (valid == 0) return RESULT_ERROR;
    
    dev->offset = sum / valid;
    LOG_INFO("HX711 tared: offset=%d", dev->offset);
    return RESULT_OK;
}

result_t hx711_set_scale(hx711_t *dev, float scale) {
    CHECK_NULL(dev);
    dev->scale = scale;
    return RESULT_OK;
}

result_t hx711_calibrate(hx711_t *dev, float known_weight, int samples) {
    CHECK_NULL(dev);
    if (known_weight == 0) return RESULT_INVALID_PARAM;
    
    int64_t sum = 0;
    int valid = 0;
    
    for (int i = 0; i < samples; i++) {
        int32_t raw;
        if (hx711_read_raw(dev, &raw) == RESULT_OK) {
            sum += raw;
            valid++;
        }
        usleep(50000);
    }
    
    if (valid == 0) return RESULT_ERROR;
    
    float avg_raw = (float)(sum / valid) - dev->offset;
    dev->scale = avg_raw / known_weight;
    
    LOG_INFO("HX711 calibrated: scale=%.2f", dev->scale);
    return RESULT_OK;
}

void hx711_power_down(hx711_t *dev) {
    if (dev && dev->initialized) {
        gpio_write(dev->gpio_base, dev->sck_pin, 1);
        delay_us(100);
    }
}

void hx711_power_up(hx711_t *dev) {
    if (dev && dev->initialized) {
        gpio_write(dev->gpio_base, dev->sck_pin, 0);
    }
}

void hx711_close(hx711_t *dev) {
    if (dev) {
        if (dev->gpio_base) munmap((void *)dev->gpio_base, 4096);
        if (dev->mem_fd >= 0) close(dev->mem_fd);
        dev->initialized = false;
    }
}

/* ============================================================================
 * Driver Interface (uses driver_common.h infrastructure)
 * ========================================================================== */

#include "driver_common.h"

typedef struct {
    DRIVER_INSTANCE_FIELDS;  /* ops, scale, offset */
    hx711_t device;
} hx711_instance_t;

/* Forward declarations for ops table */
static result_t hx711_driver_read(void *handle, float *value);
static void hx711_driver_close(void *handle);

/* Driver operations table - exported for driver registry */
const driver_ops_t driver_hx711_ops = {
    .read = hx711_driver_read,
    .close = hx711_driver_close,
    .set_calibration = driver_set_calibration_generic,
    .name = "HX711"
};

result_t driver_hx711_init(void **handle, int dout_pin, int sck_pin, int gain) {
    hx711_instance_t *inst = calloc(1, sizeof(hx711_instance_t));
    if (!inst) return RESULT_NO_MEMORY;

    driver_instance_init_base(inst, &driver_hx711_ops);

    result_t r = hx711_init(&inst->device, dout_pin, sck_pin, gain);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }

    *handle = inst;
    return RESULT_OK;
}

static result_t hx711_driver_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);

    hx711_instance_t *inst = (hx711_instance_t *)handle;
    float raw_value;

    result_t r = hx711_read(&inst->device, &raw_value);
    if (r != RESULT_OK) return r;

    *value = driver_apply_calibration(inst, raw_value);
    return RESULT_OK;
}

/* Legacy wrapper */
result_t driver_hx711_read(void *handle, float *value) {
    return hx711_driver_read(handle, value);
}

result_t driver_hx711_tare(void *handle) {
    CHECK_NULL(handle);
    hx711_instance_t *inst = (hx711_instance_t *)handle;
    return hx711_tare(&inst->device, 10);
}

result_t driver_hx711_calibrate(void *handle, float known_weight) {
    CHECK_NULL(handle);
    hx711_instance_t *inst = (hx711_instance_t *)handle;
    return hx711_calibrate(&inst->device, known_weight, 10);
}

static void hx711_driver_close(void *handle) {
    if (handle) {
        hx711_instance_t *inst = (hx711_instance_t *)handle;
        hx711_close(&inst->device);
        free(inst);
    }
}

/* Legacy wrapper */
void driver_hx711_close(void *handle) {
    hx711_driver_close(handle);
}
