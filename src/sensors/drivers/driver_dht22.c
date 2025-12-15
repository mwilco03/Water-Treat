/**
 * @file driver_dht22.c
 * @brief DHT22/AM2302 Temperature and Humidity sensor driver (GPIO)
 */

#include "common.h"
#include "utils/logger.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <sched.h>

#define DHT22_TIMEOUT_US    100
#define DHT22_MIN_INTERVAL  2000  // Minimum 2 seconds between reads

// BCM2835 peripheral base addresses
#define BCM2835_PERI_BASE   0x3F000000  // RPi 2/3
#define BCM2711_PERI_BASE   0xFE000000  // RPi 4
#define GPIO_BASE_OFFSET    0x200000
#define BLOCK_SIZE          4096

typedef struct {
    int gpio_pin;
    volatile uint32_t *gpio_base;
    int mem_fd;
    bool initialized;
    float last_temperature;
    float last_humidity;
    uint64_t last_read_time;
} dht22_t;

static volatile uint32_t *map_gpio(int *fd) {
    *fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    if (*fd < 0) {
        *fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (*fd < 0) return NULL;
        
        // Detect Pi version and set base address
        uint32_t base = BCM2835_PERI_BASE;
        FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
        if (cpuinfo) {
            char line[256];
            while (fgets(line, sizeof(line), cpuinfo)) {
                if (strstr(line, "BCM2711")) {
                    base = BCM2711_PERI_BASE;
                    break;
                }
            }
            fclose(cpuinfo);
        }
        
        return (volatile uint32_t *)mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, *fd, base + GPIO_BASE_OFFSET);
    }
    
    return (volatile uint32_t *)mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, *fd, 0);
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
    if (value) {
        gpio[7] = 1 << pin;   // GPSET0
    } else {
        gpio[10] = 1 << pin;  // GPCLR0
    }
}

static int gpio_read(volatile uint32_t *gpio, int pin) {
    return (gpio[13] & (1 << pin)) ? 1 : 0;  // GPLEV0
}

static void delay_us(int us) {
    struct timespec ts = {0, us * 1000};
    nanosleep(&ts, NULL);
}

result_t dht22_init(dht22_t *dev, int gpio_pin) {
    CHECK_NULL(dev);
    
    memset(dev, 0, sizeof(*dev));
    dev->gpio_pin = gpio_pin;
    
    dev->gpio_base = map_gpio(&dev->mem_fd);
    if (!dev->gpio_base) {
        LOG_ERROR("Failed to map GPIO memory");
        return RESULT_IO_ERROR;
    }
    
    set_gpio_input(dev->gpio_base, gpio_pin);
    
    dev->initialized = true;
    LOG_INFO("DHT22 initialized on GPIO %d", gpio_pin);
    return RESULT_OK;
}

result_t dht22_read(dht22_t *dev, float *temperature, float *humidity) {
    CHECK_NULL(dev);
    
    if (!dev->initialized) return RESULT_NOT_INITIALIZED;
    
    uint64_t now = get_time_ms();
    if (now - dev->last_read_time < DHT22_MIN_INTERVAL) {
        if (temperature) *temperature = dev->last_temperature;
        if (humidity) *humidity = dev->last_humidity;
        return RESULT_OK;
    }
    
    // Set high priority for timing-critical section
    struct sched_param sp = {.sched_priority = 50};
    sched_setscheduler(0, SCHED_FIFO, &sp);
    
    uint8_t data[5] = {0};
    int pin = dev->gpio_pin;
    volatile uint32_t *gpio = dev->gpio_base;
    
    // Send start signal
    set_gpio_output(gpio, pin);
    gpio_write(gpio, pin, 0);
    delay_us(1100);  // Hold low for >1ms
    gpio_write(gpio, pin, 1);
    delay_us(30);
    set_gpio_input(gpio, pin);
    
    // Wait for sensor response
    int timeout = DHT22_TIMEOUT_US;
    while (gpio_read(gpio, pin) && --timeout > 0) delay_us(1);
    if (timeout <= 0) goto timeout_error;
    
    timeout = DHT22_TIMEOUT_US;
    while (!gpio_read(gpio, pin) && --timeout > 0) delay_us(1);
    if (timeout <= 0) goto timeout_error;
    
    timeout = DHT22_TIMEOUT_US;
    while (gpio_read(gpio, pin) && --timeout > 0) delay_us(1);
    if (timeout <= 0) goto timeout_error;
    
    // Read 40 bits
    for (int i = 0; i < 40; i++) {
        timeout = DHT22_TIMEOUT_US;
        while (!gpio_read(gpio, pin) && --timeout > 0) delay_us(1);
        if (timeout <= 0) goto timeout_error;
        
        delay_us(35);  // Wait past the 26-28us for '0'
        
        int bit = gpio_read(gpio, pin) ? 1 : 0;
        data[i / 8] = (data[i / 8] << 1) | bit;
        
        if (bit) {
            timeout = DHT22_TIMEOUT_US;
            while (gpio_read(gpio, pin) && --timeout > 0) delay_us(1);
        }
    }
    
    // Restore normal priority
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    
    // Verify checksum
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        LOG_WARNING("DHT22 checksum error");
        return RESULT_ERROR;
    }
    
    // Parse data
    uint16_t raw_humidity = (data[0] << 8) | data[1];
    uint16_t raw_temp = (data[2] << 8) | data[3];
    
    float h = raw_humidity / 10.0f;
    float t = (raw_temp & 0x7FFF) / 10.0f;
    if (raw_temp & 0x8000) t = -t;  // Negative temperature
    
    dev->last_humidity = h;
    dev->last_temperature = t;
    dev->last_read_time = get_time_ms();
    
    if (humidity) *humidity = h;
    if (temperature) *temperature = t;
    
    return RESULT_OK;

timeout_error:
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
    LOG_WARNING("DHT22 timeout");
    return RESULT_TIMEOUT;
}

void dht22_close(dht22_t *dev) {
    if (dev) {
        if (dev->gpio_base) {
            munmap((void *)dev->gpio_base, BLOCK_SIZE);
        }
        if (dev->mem_fd >= 0) {
            close(dev->mem_fd);
        }
        dev->initialized = false;
        LOG_DEBUG("DHT22 closed");
    }
}

/* Driver interface wrapper */
typedef struct {
    dht22_t device;
    bool read_humidity;  // false = temperature, true = humidity
    float scale;
    float offset;
} dht22_instance_t;

result_t driver_dht22_init(void **handle, int gpio_pin, bool read_humidity) {
    dht22_instance_t *inst = calloc(1, sizeof(dht22_instance_t));
    if (!inst) return RESULT_NO_MEMORY;
    
    result_t r = dht22_init(&inst->device, gpio_pin);
    if (r != RESULT_OK) {
        free(inst);
        return r;
    }
    
    inst->read_humidity = read_humidity;
    inst->scale = 1.0f;
    inst->offset = 0.0f;
    
    *handle = inst;
    return RESULT_OK;
}

result_t driver_dht22_read(void *handle, float *value) {
    CHECK_NULL(handle);
    CHECK_NULL(value);
    
    dht22_instance_t *inst = (dht22_instance_t *)handle;
    float temp, hum;
    
    result_t r = dht22_read(&inst->device, &temp, &hum);
    if (r != RESULT_OK) return r;
    
    *value = inst->read_humidity ? hum : temp;
    *value = *value * inst->scale + inst->offset;
    
    return RESULT_OK;
}

result_t driver_dht22_read_both(void *handle, float *temperature, float *humidity) {
    CHECK_NULL(handle);
    
    dht22_instance_t *inst = (dht22_instance_t *)handle;
    return dht22_read(&inst->device, temperature, humidity);
}

result_t driver_dht22_set_calibration(void *handle, float scale, float offset) {
    CHECK_NULL(handle);
    dht22_instance_t *inst = (dht22_instance_t *)handle;
    inst->scale = scale;
    inst->offset = offset;
    return RESULT_OK;
}

void driver_dht22_close(void *handle) {
    if (handle) {
        dht22_instance_t *inst = (dht22_instance_t *)handle;
        dht22_close(&inst->device);
        free(inst);
    }
}
